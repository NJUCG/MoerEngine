#include "misc/Crc32.h"
#include "misc/MMemory.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile_consumer/ProfileSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace Moer::ProfileDump;

static_assert([] {
    std::uint32_t wire_bytes = 0;
    return Detail::TryPacketWireBytes(
               std::numeric_limits<std::uint32_t>::max() - kPacketHeaderBytes, wire_bytes
           ) &&
           wire_bytes == std::numeric_limits<std::uint32_t>::max();
}());
static_assert([] {
    std::uint32_t wire_bytes = 0;
    return !Detail::TryPacketWireBytes(std::numeric_limits<std::uint32_t>::max(), wire_bytes);
}());
static_assert(
    SessionLimits{}.max_transient_materialization_bytes >= SessionLimits{}.max_logical_model_bytes,
    "default transient materialization capacity must cover the retained-model default domain"
);

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

struct PacketRange {
    std::size_t offset{0};
    std::size_t size{0};
};

class SessionBuilder {
public:
    explicit SessionBuilder(std::uint64_t _generation = 7) : generation(_generation) {}

    void Begin() {
        Moer::Array<std::uint8_t> payload;
        EncodeSessionBeginPayload(
            {
                .generation      = generation,
                .started_unix_ns = 123456789,
            },
            payload
        );
        Append(PacketType::SessionBegin, payload);
    }

    void Schema(const SchemaDescriptor& _schema) {
        SchemaAt(_schema, next_packet_index);
    }

    void SchemaAt(const SchemaDescriptor& _schema, std::uint64_t _packet_index) {
        Moer::Array<std::uint8_t> payload;
        Expect(
            EncodeSchemaPayload(_schema, limits, payload) == EncodeStatus::Ok,
            "fixture schema failed to encode"
        );
        AppendAt(PacketType::Schema, payload, _packet_index);
    }

    void Record(
        const SchemaDescriptor&         _schema,
        std::uint64_t                   _sequence,
        std::span<const FieldValueView> _values
    ) {
        Moer::Array<std::uint8_t> payload;
        Expect(
            EncodeRecordPayload(
                ComputeSchemaHash(_schema), _sequence, _schema.fields, _values, limits, payload
            ) == EncodeStatus::Ok,
            "fixture record failed to encode"
        );
        Append(PacketType::Record, payload);
        ++record_count;
    }

    void Loss(const LossNotice& _loss) {
        Moer::Array<std::uint8_t> payload;
        EncodeLossPayload(_loss, payload);
        Append(PacketType::Loss, payload);
        dropped_count += _loss.record_count;
    }

    void End(std::optional<SessionEndInfo> _override = std::nullopt) {
        const SessionEndInfo      end = _override.value_or(SessionEndInfo{
                 .generation      = generation,
                 .records_written = record_count,
                 .records_dropped = dropped_count,
        });
        Moer::Array<std::uint8_t> payload;
        EncodeSessionEndPayload(end, payload);
        Append(PacketType::SessionEnd, payload);
    }

    void Append(PacketType _type, std::span<const std::uint8_t> _payload) {
        AppendAt(_type, _payload, next_packet_index);
    }

    void AppendAt(PacketType _type, std::span<const std::uint8_t> _payload, std::uint64_t _packet_index) {
        Moer::Array<std::uint8_t> packet;
        Expect(
            WrapPacket(_type, _packet_index, _payload, limits, packet) == EncodeStatus::Ok,
            "fixture packet failed to wrap"
        );
        ranges.push_back({bytes.size(), packet.size()});
        bytes.insert(bytes.end(), packet.begin(), packet.end());
        next_packet_index = _packet_index + 1;
    }

    CodecLimits               limits{};
    std::uint64_t             generation{7};
    std::uint64_t             next_packet_index{0};
    std::uint64_t             record_count{0};
    std::uint64_t             dropped_count{0};
    std::vector<std::uint8_t> bytes{};
    std::vector<PacketRange>  ranges{};
};

SchemaDescriptor UnknownSchema() {
    return {
        .name           = "FutureCounter",
        .event_type     = "future.counter",
        .kind           = EventKind::Counter,
        .channel        = Channel::CpuThread,
        .schema_version = 9,
        .fields =
            {
                {"value", FieldType::UInt64},
                {"label", FieldType::String},
            },
    };
}

void AddCpuScope(
    SessionBuilder&  _builder,
    std::uint64_t    _sequence,
    std::uint64_t    _thread,
    std::string_view _name,
    std::uint64_t    _begin,
    std::uint64_t    _end,
    std::uint32_t    _depth
) {
    const std::array<FieldValueView, 5> values{
        _thread,
        _name,
        _begin,
        _end,
        _depth,
    };
    _builder.Record(Templates::CpuScope(), _sequence, values);
}

void AddGpuFrame(
    SessionBuilder&       _builder,
    std::uint64_t         _sequence,
    std::uint64_t         _frame_id,
    ProfileGpuFrameStatus _status,
    bool                  _valid,
    std::uint64_t         _admitted,
    std::uint64_t         _dropped,
    std::uint64_t         _errors,
    std::string_view      _reason
) {
    const std::array<FieldValueView, 7> values{
        _frame_id,
        static_cast<std::uint32_t>(_status),
        _valid,
        _admitted,
        _dropped,
        _errors,
        _reason,
    };
    _builder.Record(Templates::GpuFrame(), _sequence, values);
}

void AddGpuScope(
    SessionBuilder&       _builder,
    std::uint64_t         _sequence,
    std::uint64_t         _frame_id,
    std::uint64_t         _scope_id,
    std::uint64_t         _parent_scope_id,
    std::uint64_t         _source_order,
    std::uint64_t         _local_order,
    std::uint32_t         _logical_queue,
    std::uint32_t         _native_queue_id,
    std::uint32_t         _family_id,
    std::string_view      _name,
    ProfileGpuScopeStatus _status,
    std::uint64_t         _begin_tick,
    std::uint64_t         _end_tick,
    std::uint32_t         _valid_bits,
    double                _tick_period_ns,
    double                _total_duration_ns,
    double                _exclusive_duration_ns,
    std::uint32_t         _depth,
    std::string_view      _error_reason
) {
    const std::array<FieldValueView, 18> values{
        _frame_id,
        _scope_id,
        _parent_scope_id,
        _source_order,
        _local_order,
        _logical_queue,
        _native_queue_id,
        _family_id,
        _name,
        static_cast<std::uint32_t>(_status),
        _begin_tick,
        _end_tick,
        _valid_bits,
        _tick_period_ns,
        _total_duration_ns,
        _exclusive_duration_ns,
        _depth,
        _error_reason,
    };
    _builder.Record(Templates::GpuScope(), _sequence, values);
}

struct Loaded {
    SessionLoadResult result{};
    ProfileSession    session{};
    std::string       diagnostic{};
};

Loaded LoadChunks(
    std::span<const std::uint8_t> _bytes,
    std::span<const std::size_t>  _pattern = {},
    const SessionLoadOptions&     _options = {},
    const SessionLoadControl&     _control = {}
) {
    ProfileSessionReader reader(_options, _control);
    std::size_t          offset        = 0;
    std::size_t          pattern_index = 0;
    while (offset < _bytes.size() && reader.Result().status == SessionLoadStatus::Reading) {
        const std::size_t requested =
            _pattern.empty() ? _bytes.size() - offset : _pattern[pattern_index++ % _pattern.size()];
        const std::size_t       count = std::min(std::max<std::size_t>(requested, 1), _bytes.size() - offset);
        const SessionLoadResult fed   = reader.Feed(_bytes.subspan(offset, count));
        offset += count;
        if (fed.status == SessionLoadStatus::Reading) {
            Expect(!fed.IsTerminal(), "Reading result was incorrectly reported terminal");
        }
    }

    SessionLoadResult result = reader.Result();
    if (result.status == SessionLoadStatus::Reading) {
        result = reader.Finish();
    }
    Loaded loaded{
        .result     = result,
        .diagnostic = std::string(reader.DiagnosticMessage()),
    };
    if (result.HasUsableSession()) {
        loaded.session = reader.TakeSession();
    }
    return loaded;
}

constexpr std::uint64_t TopologyLinearWorkOracle(std::size_t _count) noexcept {
    return static_cast<std::uint64_t>(_count);
}

constexpr std::uint64_t TopologySortWorkOracle(std::size_t _count) noexcept {
    const std::uint64_t count = static_cast<std::uint64_t>(_count);
    std::uint64_t       work  = 0;
    std::size_t         width = 1;
    while (width < _count) {
        work += count;
        if (width > std::numeric_limits<std::size_t>::max() / 2) {
            break;
        }
        width *= 2;
    }
    return work;
}

constexpr std::uint64_t TopologyBinarySearchWorkOracle(std::size_t _count) noexcept {
    std::uint64_t work = 0;
    while (_count != 0) {
        ++work;
        _count /= 2;
    }
    return work;
}

constexpr std::uint64_t TopologyHeapWorkOracle(std::size_t _size) noexcept {
    std::uint64_t work = 1;
    while (_size > 1) {
        ++work;
        _size /= 2;
    }
    return work;
}

std::uint64_t MinimumPassingTopologyBudget(std::span<const std::uint8_t> _bytes) {
    const auto completes_with_budget = [&](std::uint64_t _budget) {
        SessionLoadOptions options{};
        options.limits.max_topology_work_items = _budget;
        const Loaded loaded                    = LoadChunks(_bytes, {}, options);
        if (loaded.result.status == SessionLoadStatus::Complete) {
            return true;
        }
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !loaded.result.HasUsableSession() && !loaded.session.Valid(),
            "topology budget search observed a non-budget failure or a partially published session"
        );
        return false;
    };

    std::uint64_t lower = 0;
    std::uint64_t upper = 1;
    while (!completes_with_budget(upper)) {
        Expect(
            upper <= std::numeric_limits<std::uint64_t>::max() / 2,
            "topology budget search exceeded the uint64 range"
        );
        lower = upper + 1;
        upper *= 2;
    }

    while (lower < upper) {
        const std::uint64_t middle = lower + (upper - lower) / 2;
        if (completes_with_budget(middle)) {
            upper = middle;
        } else {
            lower = middle + 1;
        }
    }
    return lower;
}

std::uint64_t
MinimumPassingTopologyFlowEdges(std::span<const std::uint8_t> _bytes, SessionLoadOptions _base_options = {}) {
    const auto completes_with_limit = [&](std::uint64_t _limit) {
        SessionLoadOptions options             = _base_options;
        options.limits.max_topology_flow_edges = _limit;
        const Loaded loaded                    = LoadChunks(_bytes, {}, options);
        if (loaded.result.HasUsableSession()) {
            return true;
        }
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::TopologyFlowEdges &&
                !loaded.result.HasUsableSession() && !loaded.session.Valid(),
            "topology flow-edge search observed a non-edge failure or a partially published session"
        );
        return false;
    };

    std::uint64_t lower = 0;
    std::uint64_t upper = 1;
    while (!completes_with_limit(upper)) {
        Expect(
            upper <= std::numeric_limits<std::uint64_t>::max() / 2,
            "topology flow-edge search exceeded the uint64 range"
        );
        lower = upper + 1;
        upper *= 2;
    }
    while (lower < upper) {
        const std::uint64_t middle = lower + (upper - lower) / 2;
        if (completes_with_limit(middle)) {
            upper = middle;
        } else {
            lower = middle + 1;
        }
    }
    return lower;
}

SessionBuilder MakeGoldenSession() {
    SessionBuilder         builder;
    const SchemaDescriptor unknown = UnknownSchema();
    builder.Begin();
    builder.Schema(Templates::CpuScope());
    builder.Schema(Templates::GpuFrame());
    builder.Schema(Templates::GpuScope());
    builder.Schema(unknown);

    // Record sequence and packet order are deliberately unrelated. CPU scope
    // sequence is not a hierarchy key, while GPU sequences still preserve the
    // producer's frame-first/pre-order emission contract.
    AddCpuScope(builder, 4, 11, "cpu-child", 120, 150, 1);
    AddCpuScope(builder, 7, 11, "cpu-parent", 100, 300, 0);
    AddCpuScope(builder, 2, 22, "cpu-other", 50, 60, 0);

    AddGpuFrame(builder, 10, 100, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
    AddGpuFrame(builder, 15, 101, ProfileGpuFrameStatus::Invalid, false, 1, 0, 1, "query failed");
    AddGpuScope(
        builder,
        12,
        100,
        2,
        1,
        5,
        1,
        0,
        3,
        7,
        "gfx-child",
        ProfileGpuScopeStatus::Ready,
        110,
        140,
        64,
        1.0,
        30.0,
        30.0,
        1,
        ""
    );
    AddGpuScope(
        builder,
        11,
        100,
        1,
        0,
        5,
        0,
        0,
        3,
        7,
        "gfx-root",
        ProfileGpuScopeStatus::Ready,
        100,
        200,
        64,
        1.0,
        100.0,
        70.0,
        0,
        ""
    );
    AddGpuScope(
        builder,
        14,
        100,
        3,
        0,
        6,
        0,
        1,
        3,
        7,
        "compute-root",
        ProfileGpuScopeStatus::Ready,
        300,
        350,
        64,
        1.0,
        50.0,
        50.0,
        0,
        ""
    );
    AddGpuScope(
        builder,
        13,
        100,
        4,
        0,
        7,
        0,
        0,
        4,
        7,
        "wrapped-root",
        ProfileGpuScopeStatus::Ready,
        0xfffffff0ull,
        0x10ull,
        32,
        2.0,
        64.0,
        64.0,
        0,
        ""
    );
    AddGpuScope(
        builder,
        16,
        101,
        1,
        0,
        8,
        0,
        0,
        3,
        7,
        "failed-root",
        ProfileGpuScopeStatus::Error,
        0,
        0,
        0,
        0.0,
        0.0,
        0.0,
        0,
        "timestamp unavailable"
    );
    const std::array<FieldValueView, 2> unknown_values{
        std::uint64_t{99},
        std::string_view("future"),
    };
    builder.Record(unknown, 1, unknown_values);

    // Emitted sequence values 4/7/8 are inside this observational interval;
    // count=2 means the interval is not an exact missing-sequence set.
    builder.Loss({
        .first_sequence = 3,
        .last_sequence  = 9,
        .record_count   = 2,
        .value_bytes    = 128,
        .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
    });
    builder.End();
    return builder;
}

const GpuScopeRecord& FindGpuScope(const ProfileSession& _session, std::string_view _name) {
    const auto found = std::find_if(
        _session.GpuScopes().begin(),
        _session.GpuScopes().end(),
        [&](const GpuScopeRecord& _scope) {
            return _session.String(_scope.name) == _name;
        }
    );
    Expect(found != _session.GpuScopes().end(), "expected GPU scope was not found");
    return *found;
}

void VerifyGolden(const Loaded& _loaded) {
    Expect(
        _loaded.result.status == SessionLoadStatus::Complete,
        "golden session did not complete (status=" +
            std::to_string(static_cast<unsigned>(_loaded.result.status)) +
            ", error=" + std::to_string(static_cast<unsigned>(_loaded.result.error_code)) +
            ", diagnostic=" + _loaded.diagnostic + ")"
    );
    Expect(_loaded.session.Valid(), "golden session model is invalid");
    const ProfileSessionSummary& summary = _loaded.session.Summary();
    Expect(summary.generation == 7, "session generation is wrong");
    Expect(summary.unique_schema_count == 4, "unique schema count is wrong");
    Expect(summary.schema_packet_count == 4, "schema packet count is wrong");
    Expect(summary.record_count == 11, "record count is wrong");
    Expect(summary.cpu_scope_count == 3, "CPU scope count is wrong");
    Expect(summary.gpu_frame_count == 2, "GPU frame count is wrong");
    Expect(summary.gpu_scope_count == 5, "GPU scope count is wrong");
    Expect(summary.unknown_record_count == 1, "unknown record count is wrong");
    Expect(
        summary.complete_gpu_frame_count == 1 && summary.invalid_gpu_frame_count == 1 &&
            summary.degraded_complete_gpu_frame_count == 0 && summary.incomplete_gpu_frame_count == 0 &&
            summary.ready_gpu_scope_count == 4 && summary.error_gpu_scope_count == 1,
        "GPU status summary is wrong"
    );
    Expect(summary.lost_record_count == 2, "loss count is wrong");
    Expect(summary.loss_notice_count == 1, "loss notice count is wrong");
    Expect(summary.orphan_cpu_scope_count == 0, "golden CPU scope became orphaned");
    Expect(summary.orphan_gpu_scope_count == 0, "golden GPU scope became orphaned");
    Expect(
        _loaded.result.input_bytes == _loaded.result.valid_prefix_bytes,
        "complete session did not consume its exact input"
    );

    Expect(_loaded.session.CpuTracks().size() == 2, "CPU track count is wrong");
    const auto cpu_parent = std::find_if(
        _loaded.session.CpuScopes().begin(),
        _loaded.session.CpuScopes().end(),
        [&](const CpuScopeRecord& _scope) {
            return _loaded.session.String(_scope.name) == "cpu-parent";
        }
    );
    const auto cpu_child = std::find_if(
        _loaded.session.CpuScopes().begin(),
        _loaded.session.CpuScopes().end(),
        [&](const CpuScopeRecord& _scope) {
            return _loaded.session.String(_scope.name) == "cpu-child";
        }
    );
    Expect(
        cpu_parent != _loaded.session.CpuScopes().end() && cpu_child != _loaded.session.CpuScopes().end(),
        "CPU hierarchy records are missing"
    );
    Expect(
        cpu_child->parent_index ==
            static_cast<std::uint64_t>(cpu_parent - _loaded.session.CpuScopes().begin()),
        "CPU hierarchy was not reconstructed"
    );

    Expect(_loaded.session.GpuTracks().size() == 3, "GPU track count is wrong");
    Expect(_loaded.session.GpuDomains().size() == 2, "GPU domain count is wrong");
    const GpuScopeRecord& graphics = FindGpuScope(_loaded.session, "gfx-root");
    const GpuScopeRecord& compute  = FindGpuScope(_loaded.session, "compute-root");
    const GpuScopeRecord& wrapped  = FindGpuScope(_loaded.session, "wrapped-root");
    const GpuScopeRecord& child    = FindGpuScope(_loaded.session, "gfx-child");
    Expect(
        graphics.domain_index == compute.domain_index,
        "aliased logical queues did not share their physical timestamp domain"
    );
    Expect(
        graphics.track_index != compute.track_index, "distinct logical queues were merged into one GPU track"
    );
    Expect(
        graphics.domain_index != wrapped.domain_index,
        "independent native queues were merged into one timestamp domain"
    );
    Expect(
        child.parent_index != kInvalidSessionIndex &&
            _loaded.session.GpuScopes()[child.parent_index].scope_id == graphics.scope_id,
        "GPU parent topology was not reconstructed"
    );
    Expect(
        wrapped.begin_tick > wrapped.end_tick && wrapped.valid_bits == 32,
        "wrapped raw GPU timestamps were rejected or rewritten"
    );
    Expect(
        graphics.logical_queue == ProfileLogicalQueue::Graphics && graphics.source_order == 5 &&
            graphics.local_order == 0 && graphics.begin_tick == 100 && graphics.end_tick == 200 &&
            graphics.valid_bits == 64 && graphics.tick_period_ns == 1.0 &&
            graphics.total_duration_ns == 100.0 && graphics.exclusive_duration_ns == 70.0 &&
            graphics.depth == 0 && child.source_order == 5 && child.local_order == 1 && child.depth == 1 &&
            child.total_duration_ns == 30.0 && compute.logical_queue == ProfileLogicalQueue::Compute &&
            wrapped.native_queue_id == 4,
        "GPU scope fields changed while materializing the session"
    );
    Expect(
        FindGpuScope(_loaded.session, "failed-root").status == ProfileGpuScopeStatus::Error,
        "GPU error scope was dropped"
    );
    const auto complete_frame = std::find_if(
        _loaded.session.GpuFrames().begin(),
        _loaded.session.GpuFrames().end(),
        [](const GpuFrameRecord& _frame) {
            return _frame.frame_id == 100;
        }
    );
    const auto invalid_frame = std::find_if(
        _loaded.session.GpuFrames().begin(),
        _loaded.session.GpuFrames().end(),
        [](const GpuFrameRecord& _frame) {
            return _frame.frame_id == 101;
        }
    );
    Expect(
        complete_frame != _loaded.session.GpuFrames().end() &&
            complete_frame->status == ProfileGpuFrameStatus::Complete && complete_frame->valid &&
            complete_frame->admitted_scope_count == 4 && complete_frame->scope_count == 4 &&
            complete_frame->error_scope_count == 0 && complete_frame->materialization_complete &&
            complete_frame->timing_topology_trusted && invalid_frame != _loaded.session.GpuFrames().end() &&
            invalid_frame->status == ProfileGpuFrameStatus::Invalid && !invalid_frame->valid &&
            invalid_frame->admitted_scope_count == 1 && invalid_frame->scope_count == 1 &&
            invalid_frame->error_scope_count == 1 && invalid_frame->materialization_complete &&
            !invalid_frame->timing_topology_trusted &&
            _loaded.session.String(invalid_frame->reason) == "query failed",
        "GPU frame counters or reason changed while materializing the session"
    );
    const GpuTimestampDomain& shared_domain = _loaded.session.GpuDomains()[graphics.domain_index];
    Expect(
        shared_domain.logical_queue_mask == (ProfileLogicalQueueBit(ProfileLogicalQueue::Graphics) |
                                             ProfileLogicalQueueBit(ProfileLogicalQueue::Compute)) &&
            shared_domain.timing_capability_trusted && shared_domain.valid_bits == 64 &&
            shared_domain.tick_period_ns == 1.0 && shared_domain.ready_scope_count == 3,
        "GPU physical-domain metadata is wrong"
    );
    Expect(
        _loaded.session.Schemas().size() == 4 && _loaded.session.SchemaFields().size() == 32,
        "schema descriptors were not retained"
    );
}

void TestGoldenSessionAndArbitraryChunking() {
    const SessionBuilder golden = MakeGoldenSession();
    VerifyGolden(LoadChunks(golden.bytes));

    const std::array<std::size_t, 1> one_byte{1};
    VerifyGolden(LoadChunks(golden.bytes, one_byte));

    const std::array<std::size_t, 8> mixed{
        31,
        1,
        2,
        3,
        5,
        17,
        64,
        257,
    };
    VerifyGolden(LoadChunks(golden.bytes, mixed));
}

void TestEnvelopeSchemaAndSequenceContracts() {
    {
        SessionBuilder builder = MakeGoldenSession();
        builder.bytes.push_back(0xaa);
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "trailing byte after SessionEnd was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.SchemaAt(UnknownSchema(), 3);
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(loaded.result.status == SessionLoadStatus::ProtocolViolation, "packet index gap was accepted");
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Begin();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "duplicate SessionBegin was accepted"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        builder.Schema(unknown);
        builder.Schema(unknown);
        const std::array<FieldValueView, 2> values{
            std::uint64_t{1},
            std::string_view("x"),
        };
        builder.Record(unknown, 1, values);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().schema_packet_count == 2 &&
                loaded.session.Summary().unique_schema_count == 1,
            "exact duplicate schema was not handled deterministically"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        const std::array<FieldValueView, 2> values{
            std::uint64_t{1},
            std::string_view("x"),
        };
        builder.Record(unknown, 1, values);
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.codec_status == DecodeStatus::UnknownSchema,
            "record referencing an unregistered schema was accepted"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        builder.Schema(unknown);
        const std::array<FieldValueView, 2> first{
            std::uint64_t{1},
            std::string_view("a"),
        };
        const std::array<FieldValueView, 2> second{
            std::uint64_t{2},
            std::string_view("b"),
        };
        builder.Record(unknown, 5, first);
        builder.Record(unknown, 5, second);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "duplicate record sequence was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = 1,
            .records_dropped = 0,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "incorrect SessionEnd totals were accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = 0,
            .records_dropped = 1,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().session_end_records_dropped == 1 &&
                loaded.session.Summary().lost_record_count == 0 &&
                loaded.session.Summary().unnotified_drop_count == 1,
            "producer drops without Loss packets were rejected or misclassified"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 2,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = 0,
            .records_dropped = 1,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::SessionEndTotalsMismatch,
            "SessionEnd drop total smaller than noticed losses was accepted"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        builder.Schema(unknown);
        const std::array<FieldValueView, 2> values{
            std::uint64_t{1},
            std::string_view("observed"),
        };
        builder.Record(unknown, 1, values);
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 1,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::RecordSequenceInvalid,
            "a Loss notice reused an observed record sequence"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 5,
            .record_count   = 3,
            .value_bytes    = 24,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 4,
            .record_count   = 3,
            .value_bytes    = 24,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::Oversized),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::RecordSequenceInvalid,
            "overlapping Loss hulls reused their only residual sequence"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        builder.Schema(unknown);
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 3,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        const std::array<FieldValueView, 2> values{
            std::uint64_t{2},
            std::string_view("inside-loss-hull"),
        };
        builder.Record(unknown, 2, values);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().lost_record_count == 2,
            "an observed record inside a Loss hull was mistaken for a lost endpoint"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 3,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::Oversized),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().lost_record_count == 3,
            "distinct overlapping Loss notices were rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 0,
            .last_sequence  = 0,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::LossPayloadInvalid,
            "Loss notice claimed reserved record sequence zero"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 1,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::LossPayloadInvalid,
            "single-record Loss notice claimed different minimum and maximum sequences"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Loss({
            .first_sequence = 100,
            .last_sequence  = 100,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::Oversized),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete,
            "v3 consumer invented a finite sequence frontier absent from the wire"
        );
    }
    {
        SessionBuilder         builder;
        const SchemaDescriptor unknown = UnknownSchema();
        builder.Begin();
        builder.Schema(unknown);
        const std::array<FieldValueView, 2> values{
            std::uint64_t{2},
            std::string_view("post-validation-gap"),
        };
        builder.Record(unknown, 2, values);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete,
            "unreported post-reservation validation failure was treated as a sequence violation"
        );
    }
}

void TestChecksumAndForensicTruncation() {
    {
        const SessionBuilder golden = MakeGoldenSession();
        SessionLoadOptions   options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete,
            "forensic opt-in downgraded a complete session"
        );
    }
    {
        SessionBuilder    builder = MakeGoldenSession();
        const PacketRange record  = builder.ranges[5];
        builder.bytes[record.offset + kPacketHeaderBytes] ^= 0x80;
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::CorruptData &&
                loaded.result.codec_status == DecodeStatus::ChecksumMismatch,
            "payload CRC failure was downgraded to forensic data"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                loaded.result.incomplete_reason == SessionIncompleteReason::MissingSessionEnd &&
                loaded.session.Valid(),
            "clean prefix without SessionEnd was not exposed for forensics"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(UnknownSchema());
        builder.bytes.resize(builder.ranges[1].offset + kPacketHeaderBytes - 1);
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                loaded.result.incomplete_reason == SessionIncompleteReason::TruncatedHeader,
            "truncated header was classified incorrectly"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(UnknownSchema());
        builder.bytes.resize(builder.bytes.size() - 1);
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                loaded.result.incomplete_reason == SessionIncompleteReason::TruncatedPayload,
            "truncated payload was classified incorrectly"
        );
    }
    {
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks({}, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::CorruptData && !loaded.result.HasUsableSession(),
            "empty input was exposed as a forensic session"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 9, "forensic-child", 10, 20, 1);

        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                loaded.session.Summary().orphan_cpu_scope_count == 1 &&
                loaded.result.incomplete_byte_offset == builder.bytes.size() &&
                loaded.result.incomplete_packet_index == builder.next_packet_index,
            "forensic CPU topology did not retain a clean orphaned prefix"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 3, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            3,
            3,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "forensic-gpu-child",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            1,
            ""
        );

        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                loaded.session.Summary().orphan_gpu_scope_count == 1,
            "forensic GPU topology did not retain a clean partial frame"
        );
    }
}

void TestSemanticTopologyAndLossRecovery() {
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "bad", 20, 10, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation, "reversed CPU interval was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "crossing-a", 0, 10, 0);
        AddCpuScope(builder, 2, 1, "crossing-b", 5, 15, 0);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "Loss downgraded crossing CPU intervals"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "same-depth-parent", 0, 20, 0);
        AddCpuScope(builder, 2, 1, "same-depth-child", 5, 10, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "same-depth nested CPU intervals were accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "adjacent-a", 0, 10, 0);
        AddCpuScope(builder, 2, 1, "adjacent-b", 10, 20, 0);
        AddCpuScope(builder, 4, 2, "zero-parent", 0, 10, 0);
        AddCpuScope(builder, 3, 2, "zero-child", 10, 10, 1);
        builder.End();
        const Loaded loaded     = LoadChunks(builder.bytes);
        const auto   zero_child = std::find_if(
            loaded.session.CpuScopes().begin(),
            loaded.session.CpuScopes().end(),
            [&](const CpuScopeRecord& _scope) {
                return loaded.session.String(_scope.name) == "zero-child";
            }
        );
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                zero_child != loaded.session.CpuScopes().end() &&
                zero_child->parent_index != kInvalidSessionIndex,
            "adjacent roots or a zero-duration CPU child were rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 2, 1, "ending-parent", 0, 10, 0);
        AddCpuScope(builder, 1, 1, "boundary-child", 10, 10, 1);
        AddCpuScope(builder, 3, 1, "starting-root", 10, 20, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        const auto   child  = std::find_if(
            loaded.session.CpuScopes().begin(),
            loaded.session.CpuScopes().end(),
            [&](const CpuScopeRecord& _scope) {
                return loaded.session.String(_scope.name) == "boundary-child";
            }
        );
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                child != loaded.session.CpuScopes().end() && child->parent_index != kInvalidSessionIndex &&
                loaded.session.String(loaded.session.CpuScopes()[child->parent_index].name) ==
                    "ending-parent",
            "CPU record sequence did not keep a zero-duration child with its ending parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "ending-root", 0, 10, 0);
        AddCpuScope(builder, 2, 1, "boundary-child", 10, 10, 1);
        AddCpuScope(builder, 3, 1, "starting-parent", 10, 20, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        const auto   child  = std::find_if(
            loaded.session.CpuScopes().begin(),
            loaded.session.CpuScopes().end(),
            [&](const CpuScopeRecord& _scope) {
                return loaded.session.String(_scope.name) == "boundary-child";
            }
        );
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                child != loaded.session.CpuScopes().end() && child->parent_index != kInvalidSessionIndex &&
                loaded.session.String(loaded.session.CpuScopes()[child->parent_index].name) ==
                    "starting-parent",
            "CPU record sequence did not attach a zero-duration child to its starting parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 4, 1, "root", 0, 100, 0);
        AddCpuScope(builder, 1, 1, "ended-child", 0, 50, 1);
        AddCpuScope(builder, 2, 1, "orphan-grandchild", 50, 60, 2);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 1,
            "a positive-duration CPU orphan after an adjacent ended scope was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "deep-orphan", 0, 10, 5);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 1,
            "a lifecycle-tail CPU orphan was charged as a ProfileDump loss"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "thread-one-orphan", 0, 10, 1);
        AddCpuScope(builder, 2, 2, "thread-two-orphan", 0, 10, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 2,
            "thread-local lifecycle-tail CPU orphans were rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 3, 1, "root-one", 0, 10, 0);
        AddCpuScope(builder, 1, 1, "root-one-orphan", 1, 2, 2);
        AddCpuScope(builder, 6, 1, "root-two", 10, 20, 0);
        AddCpuScope(builder, 4, 1, "root-two-orphan", 11, 12, 2);
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = builder.record_count,
            .records_dropped = 1,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeParentMissing,
            "an unnotified drop fabricated sequence-backed CPU parent evidence"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 3, 1, "root-one", 0, 10, 0);
        AddCpuScope(builder, 1, 1, "root-one-orphan", 1, 2, 2);
        AddCpuScope(builder, 6, 1, "root-two", 10, 20, 0);
        AddCpuScope(builder, 4, 1, "root-two-orphan", 11, 12, 2);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 5,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 2,
            "two CPU drops did not cover two independent observed parent subtrees"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 6, 1, "root", 0, 100, 0);
        AddCpuScope(builder, 1, 1, "orphan-before-barrier", 0, 10, 2);
        AddCpuScope(builder, 3, 1, "observed-depth-one-barrier", 10, 20, 1);
        AddCpuScope(builder, 4, 1, "orphan-after-barrier", 20, 30, 2);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 5,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 2,
            "two CPU drops did not cover gap runs separated by an observed barrier"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 2, 1, "observed-root", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "unbudgeted-depth-two", 1, 2, 2);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "an observed CPU ancestor gap was treated as a lifecycle-tail orphan"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 2, 1, "observed-root", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "slotless-depth-two", 1, 2, 2);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "a missing CPU parent had no sequence slot between child and ancestor"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 3, 1, "observed-root", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "depth-two", 1, 2, 2);
        builder.Loss({
            .first_sequence = 4,
            .last_sequence  = 4,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeParentMissing,
            "Loss outside the CPU parent sequence interval was reused as topology evidence"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 0, 1, "zero-sequence", 0, 10, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::RecordSequenceInvalid,
            "reserved record sequence zero was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 100, 1, "forged-sparse-root", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "forged-sparse-child", 1, 2, 2);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 1,
            "unreported post-reservation failures were mistaken for an encoded sequence frontier"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 1, 1, "early-emitted-root", 0, 20, 0);
        AddCpuScope(builder, 2, 1, "strictly-contained-zero", 10, 10, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "a strictly contained zero-duration CPU child outlived its observed parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 2, 1, "earlier-root", 0, 10, 0);
        AddCpuScope(builder, 1, 1, "later-root", 10, 20, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "adjacent CPU roots moved backward in record sequence"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 3, 1, "root", 0, 30, 0);
        AddCpuScope(builder, 2, 1, "earlier-sibling", 0, 10, 1);
        AddCpuScope(builder, 1, 1, "later-sibling", 10, 20, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::CpuScopeTopologyInvalid,
            "CPU siblings moved backward in record sequence"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 4, 1, "ending-root", 0, 10, 0);
        AddCpuScope(builder, 1, 1, "early-depth-two", 1, 2, 2);
        AddCpuScope(builder, 2, 1, "boundary-depth-two", 10, 10, 2);
        AddCpuScope(builder, 5, 1, "starting-root", 10, 20, 0);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 2,
            "zero-duration CPU boundary evidence failed to share its ending ancestor's missing parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        AddCpuScope(builder, 3, 1, "cpu-root", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "cpu-depth-two", 1, 2, 2);
        AddGpuFrame(builder, 4, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::RecordSequenceInvalid,
            "one ProfileDump loss was reused by CPU and GPU topology deficits"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        AddCpuScope(builder, 1, 1, "cpu-orphan", 0, 10, 1);
        AddGpuFrame(builder, 2, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_cpu_scope_count == 1 &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1,
            "a lifecycle-tail CPU orphan consumed the GPU ProfileDump loss budget"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        AddGpuScope(
            builder,
            3,
            1,
            2,
            99,
            0,
            1,
            0,
            0,
            0,
            "orphan",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "missing GPU parent without loss was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            3,
            1,
            2,
            99,
            0,
            1,
            0,
            0,
            0,
            "orphan",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_gpu_scope_count == 1,
            "loss-tolerant GPU orphan was not retained"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        AddGpuScope(
            builder,
            3,
            1,
            2,
            99,
            0,
            1,
            0,
            0,
            0,
            "orphan-a",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        AddGpuScope(
            builder,
            5,
            1,
            3,
            99,
            0,
            1,
            1,
            1,
            1,
            "orphan-b",
            ProfileGpuScopeStatus::Ready,
            3,
            4,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 4,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "one missing GPU parent was allowed to have incompatible child contracts (status=" +
                std::to_string(static_cast<unsigned>(loaded.result.status)) +
                ", error=" + std::to_string(static_cast<unsigned>(loaded.result.error_code)) +
                ", diagnostic=" + loaded.diagnostic + ")"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            2,
            99,
            0,
            0,
            0,
            0,
            0,
            "local-zero-orphan",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "a missing GPU parent was placed before child local order zero"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        AddGpuScope(
            builder,
            3,
            1,
            3,
            98,
            0,
            1,
            0,
            0,
            0,
            "deadline-one",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        AddGpuScope(
            builder,
            4,
            1,
            4,
            99,
            0,
            2,
            0,
            0,
            0,
            "deadline-two",
            ProfileGpuScopeStatus::Ready,
            3,
            4,
            64,
            1.0,
            1.0,
            1.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 5,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "two missing GPU parents reused one local-order slot"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            1,
            1,
            "a",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            64,
            1.0,
            1.0,
            1.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            0,
            1,
            0,
            1,
            1,
            1,
            "b",
            ProfileGpuScopeStatus::Ready,
            1,
            2,
            32,
            2.0,
            2.0,
            2.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation,
            "conflicting capability metadata in one physical domain was accepted"
        );
    }
    {
        struct InvalidFrameCase {
            ProfileGpuFrameStatus status;
            bool                  valid;
            std::uint64_t         admitted;
            std::uint64_t         dropped;
            std::uint64_t         errors;
        };
        const std::array<InvalidFrameCase, 3> cases{
            InvalidFrameCase{ProfileGpuFrameStatus::Complete, true, 0, 1, 0},
            InvalidFrameCase{ProfileGpuFrameStatus::Complete, true, 1, 0, 1},
            InvalidFrameCase{ProfileGpuFrameStatus::Incomplete, false, 1, 0, 1},
        };
        for (const InvalidFrameCase& invalid : cases) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            AddGpuFrame(
                builder,
                1,
                1,
                invalid.status,
                invalid.valid,
                invalid.admitted,
                invalid.dropped,
                invalid.errors,
                "invalid counters"
            );
            builder.End();
            const Loaded loaded = LoadChunks(builder.bytes);
            Expect(
                loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                    loaded.result.error_code == SessionErrorCode::GpuFrameStatusInvalid,
                "GpuFrame status/counter contradiction was accepted"
            );
        }
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "bad-duration",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            2.0,
            19.0,
            19.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "GpuScope raw ticks and duration contradiction was accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "parent",
            ProfileGpuScopeStatus::Ready,
            100,
            200,
            64,
            1.0,
            100.0,
            90.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "outside-child",
            ProfileGpuScopeStatus::Ready,
            50,
            60,
            64,
            1.0,
            10.0,
            10.0,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "complete GpuFrame accepted a child outside its parent interval"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "parent",
            ProfileGpuScopeStatus::Ready,
            100,
            200,
            64,
            1.0,
            100.0,
            30.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "child-a",
            ProfileGpuScopeStatus::Ready,
            110,
            160,
            64,
            1.0,
            50.0,
            50.0,
            1,
            ""
        );
        AddGpuScope(
            builder,
            4,
            1,
            3,
            1,
            0,
            2,
            0,
            0,
            0,
            "child-b",
            ProfileGpuScopeStatus::Ready,
            150,
            170,
            64,
            1.0,
            20.0,
            20.0,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "complete GpuFrame accepted overlapping direct children"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "root-a",
            ProfileGpuScopeStatus::Ready,
            100,
            150,
            64,
            1.0,
            50.0,
            50.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            0,
            0,
            1,
            0,
            0,
            0,
            "root-b",
            ProfileGpuScopeStatus::Ready,
            140,
            160,
            64,
            1.0,
            20.0,
            20.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "complete GpuFrame accepted overlapping same-source roots"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "wrapped-parent",
            ProfileGpuScopeStatus::Ready,
            250,
            20,
            8,
            1.0,
            26.0,
            14.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "wrapped-child-a",
            ProfileGpuScopeStatus::Ready,
            254,
            4,
            8,
            1.0,
            6.0,
            6.0,
            1,
            ""
        );
        AddGpuScope(
            builder,
            4,
            1,
            3,
            1,
            0,
            2,
            0,
            0,
            0,
            "wrapped-child-b",
            ProfileGpuScopeStatus::Ready,
            4,
            10,
            8,
            1.0,
            6.0,
            6.0,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.GpuFrames().front().timing_topology_trusted,
            "valid wrapped GPU parent/child timing was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "wrapped-root-a",
            ProfileGpuScopeStatus::Ready,
            250,
            5,
            8,
            1.0,
            11.0,
            11.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            0,
            0,
            1,
            0,
            0,
            0,
            "wrapped-root-b",
            ProfileGpuScopeStatus::Ready,
            5,
            10,
            8,
            1.0,
            5.0,
            5.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            4,
            1,
            3,
            0,
            0,
            2,
            0,
            0,
            0,
            "wrapped-root-c",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            8,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.GpuFrames().front().timing_topology_trusted,
            "adjacent same-source GPU roots across timestamp wrap were rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "wrapped-overlap-a",
            ProfileGpuScopeStatus::Ready,
            250,
            5,
            8,
            1.0,
            11.0,
            11.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            0,
            0,
            1,
            0,
            0,
            0,
            "wrapped-overlap-b",
            ProfileGpuScopeStatus::Ready,
            4,
            10,
            8,
            1.0,
            6.0,
            6.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "overlapping same-source GPU roots across timestamp wrap were accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "tolerant-parent",
            ProfileGpuScopeStatus::Ready,
            0,
            100,
            64,
            1.0,
            100.0,
            80.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "tolerant-child",
            ProfileGpuScopeStatus::Ready,
            10,
            30,
            64,
            1.00000001,
            20.0000002,
            20.0000002,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.GpuFrames().front().timing_topology_trusted,
            "producer-equivalent timestamp-period tolerance was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "bad-exclusive",
            ProfileGpuScopeStatus::Ready,
            0,
            10,
            64,
            1.0,
            10.0,
            9.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "complete GpuFrame accepted an exclusive duration that disagrees with its full child set"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "partial-parent",
            ProfileGpuScopeStatus::Ready,
            0,
            10,
            64,
            1.0,
            10.0,
            5.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "partial-child",
            ProfileGpuScopeStatus::Ready,
            0,
            6,
            64,
            1.0,
            6.0,
            6.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 4,
            .last_sequence  = 4,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "partial GpuFrame accepted observed child plus exclusive time beyond its parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Invalid, false, 2, 0, 0, "topology invalid");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "invalid-parent",
            ProfileGpuScopeStatus::Ready,
            100,
            200,
            64,
            1.0,
            100.0,
            0.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "invalid-outside-child",
            ProfileGpuScopeStatus::Ready,
            50,
            60,
            32,
            2.0,
            20.0,
            0.0,
            1,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted &&
                !loaded.session.GpuDomains().front().timing_capability_trusted,
            "producer Invalid timing/domain evidence was rejected or marked trusted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "first-root",
            ProfileGpuScopeStatus::Ready,
            0,
            10,
            8,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            4,
            1,
            3,
            0,
            0,
            2,
            0,
            0,
            0,
            "third-root",
            ProfileGpuScopeStatus::Ready,
            200,
            210,
            8,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1 &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "missing middle root caused an unsound half-range comparison"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "zero-depth-child",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "non-root zero-depth GpuScope was accepted"
        );
    }
    for (const bool forensic : {false, true}) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "bad-root-depth",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            1,
            ""
        );
        SessionLoadOptions options{};
        if (forensic) {
            options.allow_forensic_truncation = true;
        } else {
            builder.Loss({
                .first_sequence = 3,
                .last_sequence  = 3,
                .record_count   = 1,
                .value_bytes    = 8,
                .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
            });
            builder.End();
        }
        const Loaded loaded = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeRootDepthInvalid,
            "GpuScope root/depth contradiction was downgraded by Loss or forensic mode"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Invalid, false, 2, 0, 0, "topology invalid");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "surviving-root",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().invalid_gpu_frame_count == 1 &&
                loaded.session.GpuFrames().front().scope_count == 1,
            "valid producer Invalid frame with omitted unreachable scopes was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        for (std::uint64_t scope_id = 1; scope_id <= 2; ++scope_id) {
            AddGpuScope(
                builder,
                1 + scope_id,
                1,
                scope_id,
                0,
                scope_id,
                0,
                0,
                0,
                0,
                "extra-root",
                ProfileGpuScopeStatus::Ready,
                scope_id * 10,
                scope_id * 10 + 1,
                64,
                1.0,
                1.0,
                1.0,
                0,
                ""
            );
        }
        builder.Loss({
            .first_sequence = 4,
            .last_sequence  = 4,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "Loss allowed more observed GPU scopes than the frame declared"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 100, 0, 0, "");
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "one dropped record explained an unbounded complete-frame deficit"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuFrame(builder, 2, 2, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = builder.record_count,
            .records_dropped = 4,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "drop budget was reused independently by multiple complete frames"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 6,
            .record_count   = 5,
            .value_bytes    = 40,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1 &&
                loaded.session.GpuFrames().front().export_missing_scope_count == 5 &&
                !loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "a fully budgeted complete-frame deficit was not marked degraded"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Invalid, false, 1, 0, 1, "query failed");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "undeclared-ready",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = builder.record_count,
            .records_dropped = 100,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "drop budget allowed more Ready scopes than the frame declared"
        );
    }
    for (const ProfileGpuFrameStatus status :
         {ProfileGpuFrameStatus::Incomplete, ProfileGpuFrameStatus::Invalid}) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(
            builder,
            1,
            1,
            status,
            false,
            1,
            status == ProfileGpuFrameStatus::Incomplete ? 1 : 0,
            0,
            "untrusted frame"
        );
        AddGpuScope(
            builder,
            2,
            1,
            2,
            99,
            0,
            1,
            0,
            0,
            0,
            "impossible-orphan",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            0.0,
            1,
            ""
        );
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "untrusted GpuFrame had more missing parents than missing admitted scopes"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 1, 0, "RHI drops");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "surviving-incomplete",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            0.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            0,
            0,
            1,
            0,
            0,
            0,
            "conflicting-incomplete",
            ProfileGpuScopeStatus::Ready,
            30,
            35,
            32,
            2.0,
            10.0,
            0.0,
            0,
            ""
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.GpuFrames().front().export_missing_scope_count == 2 &&
                !loaded.session.GpuFrames().front().timing_topology_trusted &&
                !loaded.session.GpuDomains().front().timing_capability_trusted,
            "GpuFrame v1 Incomplete producer omissions/domain conflicts were rejected or trusted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "surviving-root",
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            0,
            ""
        );
        builder.End(SessionEndInfo{
            .generation      = builder.generation,
            .records_written = builder.record_count,
            .records_dropped = 1,
        });
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "an unnotified drop was treated as sequence-backed GPU deficit evidence"
        );
    }
    const auto add_ready_scope = [](SessionBuilder&  _builder,
                                    std::uint64_t    _sequence,
                                    std::uint64_t    _frame_id,
                                    std::uint64_t    _scope_id,
                                    std::uint64_t    _parent_scope_id,
                                    std::uint64_t    _source_order,
                                    std::uint64_t    _local_order,
                                    std::string_view _name,
                                    std::uint64_t    _begin_tick,
                                    std::uint64_t    _end_tick,
                                    std::uint32_t    _depth) {
        AddGpuScope(
            _builder,
            _sequence,
            _frame_id,
            _scope_id,
            _parent_scope_id,
            _source_order,
            _local_order,
            0,
            0,
            0,
            _name,
            ProfileGpuScopeStatus::Ready,
            _begin_tick,
            _end_tick,
            64,
            1.0,
            static_cast<double>(_end_tick - _begin_tick),
            static_cast<double>(_end_tick - _begin_tick),
            _depth,
            ""
        );
    };
    const auto add_ready_scope_bits = [](SessionBuilder&  _builder,
                                         std::uint64_t    _sequence,
                                         std::uint64_t    _frame_id,
                                         std::uint64_t    _scope_id,
                                         std::uint64_t    _parent_scope_id,
                                         std::uint64_t    _source_order,
                                         std::uint64_t    _local_order,
                                         std::string_view _name,
                                         std::uint64_t    _begin_tick,
                                         std::uint64_t    _end_tick,
                                         std::uint32_t    _valid_bits,
                                         double           _total_duration,
                                         double           _exclusive_duration,
                                         std::uint32_t    _depth) {
        AddGpuScope(
            _builder,
            _sequence,
            _frame_id,
            _scope_id,
            _parent_scope_id,
            _source_order,
            _local_order,
            0,
            0,
            0,
            _name,
            ProfileGpuScopeStatus::Ready,
            _begin_tick,
            _end_tick,
            _valid_bits,
            1.0,
            _total_duration,
            _exclusive_duration,
            _depth,
            ""
        );
    };
    const auto add_queue_loss =
        [](SessionBuilder& _builder, std::uint64_t _first_sequence, std::uint64_t _record_count) {
            _builder.Loss({
                .first_sequence = _first_sequence,
                .last_sequence  = _first_sequence + _record_count - 1,
                .record_count   = _record_count,
                .value_bytes    = _record_count * 8,
                .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
            });
        };
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 1, "root-after-hole", 10, 20, 0);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "a Complete GPU source started after an unexplained local-order hole"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 3, 1, 1, 0, 0, 1, "source-zero-root", 10, 20, 0);
        add_ready_scope(builder, 5, 1, 2, 0, 1, 1, "source-one-root", 30, 40, 0);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "one frame-level deficit was reused by two GPU source local-order holes"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "depth-two-orphan", 10, 20, 2);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "one missing GPU record explained both a parent and its hidden root"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "budgeted-depth-two-orphan", 10, 20, 2);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 3,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1 &&
                loaded.session.GpuFrames().front().export_missing_scope_count == 2,
            "a fully budgeted hidden GPU ancestor chain was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "observed-root", 0, 100, 0);
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "orphan-with-observed-root", 10, 20, 2);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1 &&
                loaded.session.Summary().orphan_gpu_scope_count == 1,
            "an observed GPU root was not reused by a missing parent chain"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "expired-observed-root", 0, 10, 0);
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "orphan-after-root", 20, 30, 2);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "an expired observed GPU root was reused as an orphan subtree ancestor"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 2, 0, "RHI drops");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "early-root", 0, 100, 0);
        add_ready_scope(builder, 3, 1, 2, 1, 0, 4, "late-depth-one", 10, 20, 1);
        add_ready_scope(builder, 4, 1, 4, 99, 0, 5, "depth-three-orphan", 30, 40, 3);
        builder.Loss({
            .first_sequence = 5,
            .last_sequence  = 5,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuFrameTotalsMismatch,
            "a late observed GPU ancestor was reused before a missing parent's latest slot"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 8, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "prefix-root-zero", 0, 1, 0);
        add_ready_scope(builder, 3, 1, 2, 0, 0, 1, "prefix-root-one", 2, 3, 0);
        add_ready_scope(builder, 4, 1, 3, 0, 0, 2, "prefix-root-two", 4, 5, 0);
        add_ready_scope(builder, 5, 1, 4, 0, 0, 3, "prefix-root-three", 6, 7, 0);
        add_ready_scope(builder, 7, 1, 6, 99, 0, 5, "prefix-depth-three", 8, 9, 3);
        add_ready_scope(builder, 9, 1, 7, 0, 0, 7, "post-child-hole-root", 10, 11, 0);
        builder.Loss({
            .first_sequence = 6,
            .last_sequence  = 8,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "a GPU child borrowed hidden-ancestor capacity from a later local-order hole"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 6, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "deadline-root-zero", 0, 1, 0);
        add_ready_scope(builder, 3, 1, 2, 0, 0, 1, "deadline-root-one", 2, 3, 0);
        add_ready_scope(builder, 5, 1, 4, 99, 0, 3, "early-depth-three", 4, 5, 3);
        add_ready_scope(builder, 7, 1, 6, 100, 0, 5, "late-depth-two", 6, 7, 2);
        builder.Loss({
            .first_sequence = 4,
            .last_sequence  = 6,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "a later missing GPU parent was reused before its own scheduling deadline"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 7, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "release-root", 0, 200, 0);
        add_ready_scope(builder, 6, 1, 2, 1, 0, 4, "release-anchor", 10, 100, 1);
        add_ready_scope(builder, 8, 1, 3, 99, 0, 6, "release-orphan", 20, 30, 4);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 7,
            .record_count   = 4,
            .value_bytes    = 32,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "GPU ancestry borrowed local-order holes from before its observed anchor"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 7, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "barrier-root", 0, 100, 0);
        add_ready_scope(builder, 5, 1, 3, 101, 0, 3, "barrier-orphan-one", 10, 20, 3);
        add_ready_scope(builder, 6, 1, 4, 1, 0, 4, "observed-depth-one-barrier", 40, 60, 1);
        add_ready_scope(builder, 8, 1, 6, 102, 0, 6, "barrier-orphan-two", 80, 90, 3);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 7,
            .record_count   = 3,
            .value_bytes    = 24,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "GPU hidden ancestors were shared across an observed same-depth timing barrier"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "crossing-before-hole", 0, 100, 0);
        add_ready_scope(builder, 4, 1, 2, 0, 0, 2, "crossing-after-hole", 50, 150, 0);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 3,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a local-order hole concealed crossing Complete GPU roots"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "replacement-root", 0, 100, 0);
        add_ready_scope(builder, 5, 1, 3, 200, 0, 3, "child-of-missing-p", 10, 20, 3);
        add_ready_scope(builder, 6, 1, 4, 100, 0, 4, "child-of-missing-q", 30, 40, 2);
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 4,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().orphan_gpu_scope_count == 2 &&
                loaded.session.GpuFrames().front().export_missing_scope_count == 2,
            "a later direct missing GPU parent did not replace an earlier anonymous ancestor demand"
        );
    }
    for (const std::uint32_t timing_case : {0u, 1u, 2u}) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 3, 1, 2, 99, 0, 1, "missing-parent-child-a", 10, 20, 1);
        const std::uint64_t second_begin = timing_case == 0 ? 20 : timing_case == 1 ? 15 : 5;
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "missing-parent-child-b", second_begin, 30, 1);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 2,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded       = LoadChunks(builder.bytes);
        const bool   valid_timing = timing_case == 0;
        Expect(
            valid_timing ? loaded.result.status == SessionLoadStatus::Complete &&
                               loaded.session.Summary().orphan_gpu_scope_count == 2 :
                           loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                               loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            valid_timing ? "adjacent observed children of one missing GPU parent were rejected" :
                           "overlapping or backward observed children of one missing GPU parent were accepted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "wide-root", 0, 220, 8, 220.0, 218.0, 0);
        add_ready_scope_bits(builder, 3, 1, 2, 1, 0, 1, "wide-child-a", 0, 1, 8, 1.0, 1.0, 1);
        add_ready_scope_bits(builder, 4, 1, 3, 1, 0, 2, "wide-child-b", 200, 201, 8, 1.0, 1.0, 1);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "internal GPU siblings were incorrectly subjected to the root half-range rule"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope_bits(builder, 3, 1, 2, 99, 0, 1, "wide-missing-child-a", 0, 1, 8, 1.0, 1.0, 1);
        add_ready_scope_bits(builder, 4, 1, 3, 99, 0, 2, "wide-missing-child-b", 200, 201, 8, 1.0, 1.0, 1);
        add_queue_loss(builder, 2, 1);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a valid wide envelope under one missing GPU parent was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope_bits(builder, 3, 1, 2, 99, 0, 1, "cycle-child-a", 0, 1, 8, 1.0, 1.0, 1);
        add_ready_scope_bits(builder, 4, 1, 3, 99, 0, 2, "cycle-child-b", 100, 101, 8, 1.0, 1.0, 1);
        add_ready_scope_bits(builder, 5, 1, 4, 99, 0, 3, "cycle-child-c", 200, 201, 8, 1.0, 1.0, 1);
        add_ready_scope_bits(builder, 6, 1, 5, 99, 0, 4, "cycle-child-d", 44, 45, 8, 1.0, 1.0, 1);
        add_queue_loss(builder, 2, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "children of one missing GPU parent spanned more than one timestamp cycle"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope_bits(builder, 3, 1, 2, 99, 0, 1, "wrapped-missing-child-a", 250, 5, 8, 11.0, 11.0, 1);
        add_ready_scope_bits(builder, 4, 1, 3, 99, 0, 2, "wrapped-missing-child-b", 5, 10, 8, 5.0, 5.0, 1);
        add_queue_loss(builder, 2, 1);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a valid wrapped envelope under one missing GPU parent was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "bridge-root-a", 0, 1, 8, 1.0, 1.0, 0);
        add_ready_scope_bits(builder, 4, 1, 2, 0, 0, 2, "bridge-root-b", 255, 0, 8, 1.0, 1.0, 0);
        add_queue_loss(builder, 3, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "one lost root bridged more than two legal half-range timestamp steps"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "three-step-root-a", 0, 100, 8, 100.0, 100.0, 0);
        add_ready_scope_bits(builder, 5, 1, 2, 0, 0, 3, "three-step-root-b", 50, 60, 8, 10.0, 10.0, 0);
        add_queue_loss(builder, 3, 2);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "two lost roots could not bridge a legal three-step timestamp wrap"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "endpoint-root", 0, 100, 64, 100.0, 50.0, 0);
        add_ready_scope(builder, 3, 1, 2, 1, 0, 1, "endpoint-sibling-a", 0, 50, 1);
        add_ready_scope(builder, 5, 1, 4, 99, 0, 3, "endpoint-orphan", 50, 50, 2);
        add_queue_loss(builder, 4, 1);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a zero-duration orphan at a completed sibling boundary chose that sibling as its parent"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "endpoint-grandparent", 0, 50, 0);
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "endpoint-grandchild", 50, 50, 2);
        add_queue_loss(builder, 3, 1);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a genuine zero-duration grandchild at its observed ancestor endpoint was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "contract-barrier-root", 0, 100, 0);
        add_ready_scope(builder, 4, 1, 3, 99, 0, 2, "contract-child-a", 10, 20, 2);
        add_ready_scope(builder, 5, 1, 4, 1, 0, 3, "contract-barrier", 30, 40, 1);
        add_ready_scope(builder, 6, 1, 5, 99, 0, 4, "contract-child-b", 50, 60, 2);
        add_queue_loss(builder, 3, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "one missing GPU parent crossed an observed same-depth hierarchy barrier"
        );
    }
    for (const bool leave_sibling_hole : {false, true}) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope(builder, 2, 1, 1, 0, 0, 0, "virtual-sibling-root", 0, 100, 0);
        const std::uint64_t first_child_order     = leave_sibling_hole ? 2 : 3;
        const std::uint64_t second_child_order    = leave_sibling_hole ? 4 : 4;
        const std::uint64_t first_child_sequence  = leave_sibling_hole ? 4 : 5;
        const std::uint64_t second_child_sequence = 6;
        add_ready_scope(
            builder,
            first_child_sequence,
            1,
            3,
            101,
            0,
            first_child_order,
            "virtual-sibling-child-a",
            10,
            20,
            2
        );
        add_ready_scope(
            builder,
            second_child_sequence,
            1,
            4,
            102,
            0,
            second_child_order,
            "virtual-sibling-child-b",
            30,
            40,
            2
        );
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = leave_sibling_hole ? 5ull : 4ull,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            leave_sibling_hole ? loaded.result.status == SessionLoadStatus::Complete :
                                 loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                                     loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            leave_sibling_hole ?
                "distinct missing GPU siblings were rejected despite a hole between their subtrees" :
                "distinct missing GPU siblings borrowed only pre-subtree holes"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 6, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "contract-lca-root", 0, 100, 64, 100.0, 50.0, 0);
        add_ready_scope(builder, 3, 1, 2, 1, 0, 1, "contract-lca-sibling", 0, 50, 1);
        add_ready_scope(builder, 6, 1, 4, 99, 0, 4, "contract-lca-zero-child", 50, 50, 3);
        add_ready_scope(builder, 7, 1, 5, 99, 0, 5, "contract-lca-late-child", 60, 70, 3);
        add_queue_loss(builder, 4, 2);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a missing-parent envelope did not choose its deepest common observed ancestor"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "virtual-chain-root-a", 0, 127, 8, 127.0, 127.0, 0);
        add_ready_scope_bits(builder, 4, 1, 3, 99, 0, 2, "virtual-chain-child", 200, 210, 8, 10.0, 10.0, 1);
        add_ready_scope_bits(builder, 5, 1, 4, 0, 0, 3, "virtual-chain-root-b", 100, 110, 8, 10.0, 10.0, 0);
        add_queue_loss(builder, 3, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a virtual root bypassed the serial root timestamp contract"
        );
    }
    for (const std::uint64_t split_loss_count : {3ull, 4ull}) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2 + split_loss_count, 0, 0, "");
        add_ready_scope_bits(builder, 4, 1, 3, 101, 0, 2, "split-root-child-a", 10, 30, 8, 20.0, 20.0, 2);
        const std::uint64_t second_child_order    = split_loss_count == 3 ? 4 : 5;
        const std::uint64_t second_child_sequence = split_loss_count == 3 ? 6 : 7;
        add_ready_scope_bits(
            builder,
            second_child_sequence,
            1,
            4,
            102,
            0,
            second_child_order,
            "split-root-child-b",
            20,
            40,
            8,
            20.0,
            20.0,
            2
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = split_loss_count == 3 ? 5ull : 6ull,
            .record_count   = split_loss_count,
            .value_bytes    = split_loss_count * 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            split_loss_count == 4 ? loaded.result.status == SessionLoadStatus::Complete :
                                    loaded.result.status == SessionLoadStatus::ProtocolViolation,
            split_loss_count == 4 ?
                "overlapping virtual-root envelopes did not split despite spare ancestry holes" :
                "overlapping virtual-root envelopes split without enough ancestry holes"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 7, 0, 0, "");
        add_ready_scope_bits(builder, 4, 1, 3, 101, 0, 2, "wide-root-child-a", 10, 20, 8, 10.0, 10.0, 2);
        add_ready_scope_bits(builder, 7, 1, 4, 102, 0, 5, "wide-root-child-b", 200, 210, 8, 10.0, 10.0, 2);
        add_ready_scope_bits(builder, 8, 1, 5, 0, 0, 6, "wide-root-successor", 220, 230, 8, 10.0, 10.0, 0);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 6,
            .record_count   = 4,
            .value_bytes    = 32,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a wide virtual envelope was not partitioned before a later observed root"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 7, 0, 0, "");
        add_ready_scope_bits(builder, 4, 1, 3, 101, 0, 2, "partition-child-a", 0, 10, 8, 10.0, 10.0, 2);
        add_ready_scope_bits(builder, 7, 1, 4, 102, 0, 5, "partition-child-b", 100, 110, 8, 10.0, 10.0, 2);
        add_ready_scope_bits(builder, 8, 1, 5, 0, 0, 6, "partition-successor", 150, 160, 8, 10.0, 10.0, 0);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 6,
            .record_count   = 4,
            .value_bytes    = 32,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                !loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "a timing-driven virtual-root partition with an internal spare hole was rejected or trusted"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 9, 0, 0, "");
        add_ready_scope_bits(
            builder, 3, 1, 2, 100, 0, 1, "future-shared-child-zero", 0, 10, 8, 10.0, 10.0, 1
        );
        add_ready_scope_bits(
            builder, 7, 1, 3, 201, 0, 5, "future-shared-child-one", 100, 105, 8, 5.0, 5.0, 3
        );
        add_ready_scope_bits(
            builder, 9, 1, 4, 202, 0, 7, "future-shared-child-two", 106, 110, 8, 4.0, 4.0, 3
        );
        add_ready_scope_bits(
            builder, 10, 1, 5, 0, 0, 8, "future-shared-successor", 150, 160, 8, 10.0, 10.0, 0
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 8,
            .record_count   = 5,
            .value_bytes    = 40,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                !loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "a future-shared missing suffix inflated an earlier virtual-root partition cost"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 9, 0, 0, "");
        add_ready_scope_bits(builder, 3, 1, 2, 100, 0, 1, "partial-split-child-zero", 0, 0, 8, 0.0, 0.0, 1);
        add_ready_scope_bits(
            builder, 6, 1, 3, 201, 0, 4, "partial-split-child-one", 100, 100, 8, 0.0, 0.0, 2
        );
        add_ready_scope_bits(
            builder, 8, 1, 4, 202, 0, 6, "partial-split-child-two", 110, 110, 8, 0.0, 0.0, 2
        );
        add_ready_scope_bits(builder, 10, 1, 5, 0, 0, 8, "partial-split-successor", 50, 60, 8, 10.0, 10.0, 0);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 9,
            .record_count   = 5,
            .value_bytes    = 40,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                !loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "a later infeasible cut rolled back an earlier valid virtual-root partition"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "occurrence-frontier-root-a", 0, 0, 8, 0.0, 0.0, 0);
        add_ready_scope_bits(
            builder, 5, 1, 3, 99, 0, 3, "occurrence-frontier-child", 125, 125, 8, 0.0, 0.0, 1
        );
        add_ready_scope_bits(
            builder, 6, 1, 4, 0, 0, 4, "occurrence-frontier-root-b", 175, 180, 8, 5.0, 5.0, 0
        );
        add_queue_loss(builder, 3, 2);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete &&
                !loaded.session.GpuFrames().front().materialization_complete &&
                !loaded.session.GpuFrames().front().timing_topology_trusted,
            "a later flexible-envelope occurrence hid an earlier valid serial timestamp witness"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 8, 0, 0, "");
        add_ready_scope_bits(
            builder, 2, 1, 1, 0, 0, 0, "partition-soundness-root-a", 0, 127, 8, 127.0, 127.0, 0
        );
        add_ready_scope_bits(
            builder, 5, 1, 3, 101, 0, 3, "partition-soundness-child-a", 255, 255, 8, 0.0, 0.0, 2
        );
        add_ready_scope_bits(
            builder, 8, 1, 4, 102, 0, 6, "partition-soundness-child-b", 255, 255, 8, 0.0, 0.0, 2
        );
        add_ready_scope_bits(
            builder, 9, 1, 5, 0, 0, 7, "partition-soundness-root-b", 150, 160, 8, 10.0, 10.0, 0
        );
        builder.Loss({
            .first_sequence = 3,
            .last_sequence  = 7,
            .record_count   = 4,
            .value_bytes    = 32,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a local-order virtual-root split erased its predecessor and successor timestamp constraints"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 7, 0, 0, "");
        add_ready_scope_bits(
            builder, 5, 1, 3, 101, 0, 3, "blocked-partition-child-a", 0, 10, 8, 10.0, 10.0, 2
        );
        add_ready_scope_bits(
            builder, 7, 1, 4, 102, 0, 5, "blocked-partition-child-b", 100, 110, 8, 10.0, 10.0, 2
        );
        add_ready_scope_bits(
            builder, 8, 1, 5, 0, 0, 6, "blocked-partition-successor", 150, 160, 8, 10.0, 10.0, 0
        );
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 6,
            .record_count   = 4,
            .value_bytes    = 32,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a virtual-root partition borrowed a spare hole before its first subtree"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(
            builder, 3, 1, 2, 99, 0, 1, "long-atomic-virtual-child", 0, 200, 8, 200.0, 200.0, 1
        );
        add_ready_scope_bits(builder, 5, 1, 3, 0, 0, 3, "long-atomic-successor", 210, 220, 8, 10.0, 10.0, 0);
        builder.Loss({
            .first_sequence = 2,
            .last_sequence  = 4,
            .record_count   = 2,
            .value_bytes    = 16,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a long atomic virtual root borrowed an optional partition before its successor"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 5, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "late-root-a", 0, 100, 8, 100.0, 100.0, 0);
        add_ready_scope_bits(builder, 5, 1, 3, 99, 0, 3, "late-root-child", 250, 0, 8, 6.0, 6.0, 1);
        add_ready_scope_bits(builder, 6, 1, 4, 0, 0, 4, "late-root-b", 0, 10, 8, 10.0, 10.0, 0);
        add_queue_loss(builder, 3, 2);
        builder.End();
        Expect(
            LoadChunks(builder.bytes).result.status == SessionLoadStatus::Complete,
            "a required virtual root was not scheduled after an optional bridge root"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "long-root", 0, 200, 8, 200.0, 200.0, 0);
        add_ready_scope_bits(builder, 5, 1, 3, 99, 0, 3, "long-root-successor", 210, 220, 8, 10.0, 10.0, 1);
        add_queue_loss(builder, 3, 2);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeTimingInvalid,
            "a root longer than one legal serial step borrowed a later optional root"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
        add_ready_scope_bits(builder, 2, 1, 1, 0, 0, 0, "closed-parent-root", 0, 100, 64, 100.0, 50.0, 0);
        add_ready_scope_bits(builder, 3, 1, 2, 1, 0, 1, "closed-parent", 0, 30, 64, 30.0, 20.0, 1);
        add_ready_scope(builder, 4, 1, 3, 1, 0, 2, "closing-sibling", 40, 60, 1);
        add_ready_scope(builder, 5, 1, 4, 2, 0, 3, "late-closed-child", 10, 20, 2);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeParentInvalid,
            "a child reopened an observed parent after a later sibling closed it"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_ready_scope(
            builder,
            2,
            1,
            1,
            0,
            0,
            std::numeric_limits<std::uint64_t>::max(),
            "overflow-local-order",
            10,
            20,
            0
        );
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                loaded.result.error_code == SessionErrorCode::GpuScopeIdentityInvalid,
            "UINT64_MAX GPU local order overflowed its source span"
        );
    }
    {
        constexpr std::uint64_t candidate_count = 32;
        constexpr std::uint64_t child_count     = 32;

        const auto make_fixture = [](std::uint32_t _child_depth) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::CpuScope());
            for (std::uint64_t index = 0; index < candidate_count; ++index) {
                AddCpuScope(builder, index + 1, 1, "ending-candidate", 10, 10, 1);
            }
            for (std::uint64_t index = 0; index < child_count; ++index) {
                AddCpuScope(
                    builder, candidate_count + index + 1, 1, "unmatched-boundary-child", 10, 10, _child_depth
                );
            }
            builder.End();
            return builder;
        };

        const SessionBuilder    adversarial        = make_fixture(2);
        const SessionBuilder    control            = make_fixture(1);
        const std::uint64_t     adversarial_budget = MinimumPassingTopologyBudget(adversarial.bytes);
        const std::uint64_t     control_budget     = MinimumPassingTopologyBudget(control.bytes);
        constexpr std::uint64_t scope_count        = candidate_count + child_count;
        const std::uint64_t     expected_index_work =
            TopologySortWorkOracle(scope_count) * 2 + TopologyLinearWorkOracle(scope_count) * 4;
        const std::uint64_t candidate_map_work = TopologyHeapWorkOracle(1) +
                                                 (candidate_count - 2) * TopologyHeapWorkOracle(2) +
                                                 (candidate_count - 1) * TopologyBinarySearchWorkOracle(1);
        const std::uint64_t control_child_map_work =
            child_count * (TopologyHeapWorkOracle(2) + TopologyBinarySearchWorkOracle(1));
        const std::uint64_t expected_control_budget =
            expected_index_work + candidate_map_work + control_child_map_work;
        Expect(
            control_budget == expected_control_budget,
            "CPU boundary-parent control disagreed with the independent indexing/container oracle"
        );
        const std::uint64_t ordered_map_delta =
            (child_count - 1) * (TopologyBinarySearchWorkOracle(2) - TopologyBinarySearchWorkOracle(1));
        Expect(
            adversarial_budget == control_budget + candidate_count * child_count + ordered_map_delta,
            "CPU boundary-parent candidate/map work was under- or over-charged"
        );
        SessionLoadOptions exact{};
        exact.limits.max_topology_work_items = adversarial_budget;
        const Loaded exact_loaded            = LoadChunks(adversarial.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::Complete &&
                exact_loaded.session.Summary().orphan_cpu_scope_count == candidate_count + child_count,
            "exact CPU boundary-parent topology budget was rejected"
        );

        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_work_items;
        const Loaded limited = LoadChunks(adversarial.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "CPU boundary-parent search escaped the session topology work budget"
        );
    }
}

void TestGpuProducerSequenceProofs() {
    const auto add_scope = [](SessionBuilder&  _builder,
                              std::uint64_t    _sequence,
                              std::uint64_t    _frame_id,
                              std::uint64_t    _scope_id,
                              std::uint64_t    _parent_scope_id,
                              std::uint64_t    _source_order,
                              std::uint64_t    _local_order,
                              std::uint32_t    _depth,
                              std::string_view _name) {
        AddGpuScope(
            _builder,
            _sequence,
            _frame_id,
            _scope_id,
            _parent_scope_id,
            _source_order,
            _local_order,
            0,
            0,
            0,
            _name,
            ProfileGpuScopeStatus::Ready,
            10,
            20,
            64,
            1.0,
            10.0,
            10.0,
            _depth,
            ""
        );
    };
    const auto add_loss =
        [](SessionBuilder& _builder, std::uint64_t _first, std::uint64_t _last, std::uint64_t _count) {
            _builder.Loss({
                .first_sequence = _first,
                .last_sequence  = _last,
                .record_count   = _count,
                .value_bytes    = _count * 8,
                .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
            });
        };
    const auto expect_rejected = [](const Loaded&    _loaded,
                                    SessionErrorCode _error,
                                    std::string_view _message,
                                    std::string_view _diagnostic = {}) {
        Expect(
            _loaded.result.status == SessionLoadStatus::ProtocolViolation &&
                _loaded.result.error_code == _error && !_loaded.result.HasUsableSession() &&
                !_loaded.session.Valid() && (_diagnostic.empty() || _loaded.diagnostic == _diagnostic),
            std::string(_message) + " (diagnostic=" + _loaded.diagnostic + ")"
        );
    };

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 3, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "scope-before-own-frame");
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "a GpuScope record preceded its owning GpuFrame record",
            "GpuFrame record sequence does not precede its GpuScope records"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(control, 2, 1, 1, 0, 0, 0, 0, "scope-after-own-frame");
        control.End();
        const Loaded loaded = LoadChunks(control.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "a GpuScope record after its owning frame was rejected"
        );
    }
    {
        const auto add_parent_child =
            [](SessionBuilder& _builder, std::uint64_t _parent_sequence, std::uint64_t _child_sequence) {
                AddGpuScope(
                    _builder,
                    _parent_sequence,
                    1,
                    1,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    "observed-parent",
                    ProfileGpuScopeStatus::Ready,
                    10,
                    30,
                    64,
                    1.0,
                    20.0,
                    10.0,
                    0,
                    ""
                );
                AddGpuScope(
                    _builder,
                    _child_sequence,
                    1,
                    2,
                    1,
                    0,
                    1,
                    0,
                    0,
                    0,
                    "observed-child",
                    ProfileGpuScopeStatus::Ready,
                    15,
                    25,
                    64,
                    1.0,
                    10.0,
                    10.0,
                    1,
                    ""
                );
            };

        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_parent_child(builder, 3, 2);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "an observed GPU parent was emitted after its child",
            "GpuScope parent record sequence does not precede its child"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_parent_child(control, 2, 3);
        control.End();
        const Loaded loaded = LoadChunks(control.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "an observed GPU parent before its child was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_scope(builder, 3, 1, 1, 0, 0, 0, 0, "later-sequence-source-zero");
        add_scope(builder, 2, 1, 2, 0, 1, 0, 0, "earlier-sequence-source-one");
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "GPU recording sources reversed producer emission order",
            "GpuScope records reverse producer emission order"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_scope(control, 2, 1, 1, 0, 0, 0, 0, "ordered-source-zero");
        add_scope(control, 3, 1, 2, 0, 1, 0, 0, "ordered-source-one");
        control.End();
        const Loaded loaded = LoadChunks(control.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "producer-ordered GPU recording sources were rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(builder, 4, 1, 1, 0, 0, 0, 0, "scope-after-next-frame");
        AddGpuFrame(builder, 3, 2, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "a GpuScope crossed the next known frame boundary",
            "GPU frame emission boundaries reverse producer frame order"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(control, 2, 1, 1, 0, 0, 0, 0, "scope-before-next-frame");
        AddGpuFrame(control, 3, 2, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        control.End();
        const Loaded loaded = LoadChunks(control.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "a GpuScope before the next known frame boundary was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 3, 0, 0, "");
        add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "source-a-root");
        add_scope(builder, 4, 1, 3, 0, 1, 0, 0, "source-b-root");
        add_loss(builder, 3, 3, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid() &&
                loaded.session.Summary().degraded_complete_gpu_frame_count == 1,
            "a Complete-frame source-tail loss between observed sources was rejected"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        AddGpuFrame(builder, 3, 3, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_scope(builder, 4, 2, 1, 0, 0, 0, 0, "missing-frame-after-next-frame");
        add_loss(builder, 2, 2, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "a missing frame emitted scopes after the next known frame record"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        AddGpuFrame(control, 4, 3, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_scope(control, 3, 2, 1, 0, 0, 0, 0, "ordered-missing-frame");
        add_loss(control, 2, 2, 1);
        control.End();
        Expect(
            LoadChunks(control.bytes).result.status == SessionLoadStatus::Complete,
            "a producer-ordered missing frame was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(builder, 3, 2, 1, 0, 0, 0, 0, "missing-next-frame-root");
        add_loss(builder, 2, 4, 2);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a Complete-frame tail crossed the record of the next missing frame"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
        add_scope(control, 4, 2, 1, 0, 0, 0, 0, "ordered-next-frame-root");
        add_loss(control, 2, 3, 2);
        control.End();
        Expect(
            LoadChunks(control.bytes).result.status == SessionLoadStatus::Complete,
            "a Complete-frame tail and following missing frame did not fit valid ordered slots"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 3, 1, 0, "RHI drops");
        add_scope(builder, 3, 1, 1, 0, 1, 0, 0, "observed-root-predecessor");
        add_scope(builder, 4, 1, 3, 99, 1, 2, 1, "child-after-missing-root");
        add_loss(builder, 2, 2, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a missing direct GPU parent was placed before its producer predecessor"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 3, 1, 0, "RHI drops");
        add_scope(control, 2, 1, 1, 0, 1, 0, 0, "ordered-root-predecessor");
        add_scope(control, 4, 1, 3, 99, 1, 2, 1, "ordered-child-after-missing-root");
        add_loss(control, 3, 3, 1);
        control.End();
        Expect(
            LoadChunks(control.bytes).result.status == SessionLoadStatus::Complete,
            "a direct GPU parent in its producer-valid slot was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 1, 0, "RHI drops");
        add_scope(builder, 3, 1, 3, 99, 1, 2, 2, "early-depth-two-child");
        add_scope(builder, 6, 1, 4, 0, 2, 0, 0, "later-unrelated-root");
        add_loss(builder, 2, 5, 2);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a hidden GPU ancestor was placed after the child that requires it"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 1, 0, "RHI drops");
        add_scope(control, 4, 1, 3, 99, 1, 2, 2, "ordered-depth-two-child");
        add_scope(control, 6, 1, 4, 0, 2, 0, 0, "ordered-later-root");
        add_loss(control, 2, 3, 2);
        control.End();
        Expect(
            LoadChunks(control.bytes).result.status == SessionLoadStatus::Complete,
            "a hidden GPU ancestor chain before its child was rejected"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_scope(builder, 4, 2, 3, 99, 1, 2, 2, "missing-frame-depth-two-child");
        add_scope(builder, 7, 2, 4, 0, 2, 0, 0, "missing-frame-later-root");
        add_loss(builder, 2, 6, 3);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a missing frame record and its parent chain reused too few pre-child slots"
        );

        SessionBuilder control;
        control.Begin();
        control.Schema(Templates::GpuFrame());
        control.Schema(Templates::GpuScope());
        AddGpuFrame(control, 1, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_scope(control, 5, 2, 3, 99, 1, 2, 2, "ordered-missing-frame-child");
        add_scope(control, 7, 2, 4, 0, 2, 0, 0, "ordered-missing-frame-root");
        add_loss(control, 2, 4, 3);
        control.End();
        Expect(
            LoadChunks(control.bytes).result.status == SessionLoadStatus::Complete,
            "a missing frame record and parent chain in valid slots were rejected"
        );
    }
    {
        const auto make_distinct_parent_fixture =
            [&](std::uint64_t _second_child_sequence, bool _with_loss, std::uint64_t _loss_last) {
                SessionBuilder builder;
                builder.Begin();
                builder.Schema(Templates::GpuFrame());
                builder.Schema(Templates::GpuScope());
                AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 0, 0, "");
                add_scope(builder, 4, 1, 2, 99, 0, 1, 1, "first-missing-parent-child");
                add_scope(builder, _second_child_sequence, 1, 3, 100, 0, 3, 1, "second-missing-parent-child");
                if (_with_loss) {
                    add_loss(builder, 2, _loss_last, 2);
                    builder.End();
                }
                return builder;
            };

        SessionBuilder no_intervening_slot = make_distinct_parent_fixture(5, true, 3);
        expect_rejected(
            LoadChunks(no_intervening_slot.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "distinct missing GPU parents crossed an observed child subtree"
        );

        SessionBuilder misplaced_loss = make_distinct_parent_fixture(6, true, 3);
        expect_rejected(
            LoadChunks(misplaced_loss.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "noticed Loss before the first child was reused for its later sibling parent"
        );

        SessionBuilder complete_control = make_distinct_parent_fixture(6, true, 5);
        const Loaded   complete_loaded  = LoadChunks(complete_control.bytes);
        Expect(
            complete_loaded.result.status == SessionLoadStatus::Complete && complete_loaded.session.Valid(),
            "distinct missing GPU parents in producer-valid Loss slots were rejected"
        );

        SessionBuilder     forensic_control = make_distinct_parent_fixture(6, false, 0);
        SessionLoadOptions forensic_options{};
        forensic_options.allow_forensic_truncation = true;
        const Loaded forensic_loaded               = LoadChunks(forensic_control.bytes, {}, forensic_options);
        Expect(
            forensic_loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                forensic_loaded.session.Valid(),
            "forensic distinct missing GPU parents with producer-valid holes were rejected"
        );
    }
    {
        const auto make_hidden_depth_fixture = [&](std::uint64_t _second_child_sequence,
                                                   std::uint64_t _loss_last) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 0, 0, "");
            add_scope(builder, 4, 1, 2, 99, 0, 1, 1, "missing-root-child");
            add_scope(builder, _second_child_sequence, 1, 3, 100, 0, 3, 2, "later-missing-depth-one-child");
            add_loss(builder, 2, _loss_last, 2);
            builder.End();
            return builder;
        };

        SessionBuilder builder = make_hidden_depth_fixture(5, 3);
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a later virtual missing task crossed a completed observed subtree"
        );

        SessionBuilder control        = make_hidden_depth_fixture(6, 5);
        const Loaded   control_loaded = LoadChunks(control.bytes);
        Expect(
            control_loaded.result.status == SessionLoadStatus::Complete && control_loaded.session.Valid(),
            "a later virtual missing task in its producer-valid slot was rejected"
        );
    }
    {
        const auto make_structural_epoch_fixture = [&](std::uint64_t _orphan_sequence,
                                                       std::uint64_t _admitted,
                                                       std::uint64_t _loss_last,
                                                       std::uint64_t _loss_count) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(
                builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, _admitted, 0, 0, "RHI drops"
            );
            add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "first-root");
            add_scope(builder, 3, 1, 2, 1, 0, 1, 1, "first-root-child");
            add_scope(builder, 4, 1, 3, 0, 0, 2, 0, "second-root");
            add_scope(builder, _orphan_sequence, 1, 6, 99, 0, 5, 3, "second-root-orphan");
            add_loss(builder, 5, _loss_last, _loss_count);
            builder.End();
            return builder;
        };

        SessionBuilder builder = make_structural_epoch_fixture(6, 5, 5, 1);
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "an observed depth from a completed root epoch hid a new hidden GPU ancestor"
        );

        SessionBuilder control        = make_structural_epoch_fixture(7, 6, 6, 2);
        const Loaded   control_loaded = LoadChunks(control.bytes);
        Expect(
            control_loaded.result.status == SessionLoadStatus::Complete && control_loaded.session.Valid(),
            "a hidden GPU ancestor after a new root epoch was rejected"
        );
    }
    {
        const auto make_local_assignment_fixture = [&](std::uint64_t _second_child_sequence,
                                                       std::uint64_t _loss_last) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 5, 0, 0, "RHI drops");
            add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "assignment-root");
            add_scope(builder, 5, 1, 3, 99, 0, 2, 2, "assignment-orphan-a");
            add_scope(builder, _second_child_sequence, 1, 5, 100, 0, 4, 1, "assignment-orphan-b");
            add_loss(builder, 3, _loss_last, 2);
            builder.End();
            return builder;
        };

        SessionBuilder builder = make_local_assignment_fixture(6, 4);
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "local EDF assignments reused a record-sequence predecessor from an earlier structural segment"
        );

        SessionBuilder control        = make_local_assignment_fixture(7, 6);
        const Loaded   control_loaded = LoadChunks(control.bytes);
        Expect(
            control_loaded.result.status == SessionLoadStatus::Complete && control_loaded.session.Valid(),
            "local EDF assignments with compatible record-sequence segments were rejected"
        );

        const std::uint64_t exact_budget = MinimumPassingTopologyBudget(control.bytes);
        Expect(
            exact_budget == 718,
            "nontrusted virtual-task topology work disagreed with the independent oracle (actual=" +
                std::to_string(exact_budget) + ")"
        );
        SessionLoadOptions exact{};
        exact.limits.max_topology_work_items = exact_budget;
        const Loaded exact_loaded            = LoadChunks(control.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::Complete && exact_loaded.session.Valid(),
            "the exact nontrusted virtual-task topology budget was rejected"
        );
        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_work_items;
        const Loaded limited = LoadChunks(control.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "nontrusted virtual-task work escaped the topology budget"
        );
    }
    {
        const auto make_spare_hole_fixture = [&](std::uint64_t _second_child_sequence,
                                                 std::uint64_t _loss_last) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 4, 2, 0, "");
            add_scope(builder, 5, 1, 2, 99, 0, 3, 1, "first-child-after-spare-holes");
            add_scope(
                builder, _second_child_sequence, 1, 3, 100, 0, 5, 1, "second-child-after-first-subtree"
            );
            add_loss(builder, 2, _loss_last, 2);
            builder.End();
            return builder;
        };

        SessionBuilder builder = make_spare_hole_fixture(6, 3);
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "spare early local-order holes hid a later sibling-parent sequence violation"
        );

        SessionBuilder control        = make_spare_hole_fixture(7, 6);
        const Loaded   control_loaded = LoadChunks(control.bytes);
        Expect(
            control_loaded.result.status == SessionLoadStatus::Complete && control_loaded.session.Valid(),
            "a sibling parent after spare local-order holes was rejected"
        );
    }
    {
        const auto make_joint_local_sequence_fixture = [&](std::uint64_t _last_child_local_order,
                                                           std::uint64_t _dropped_scope_count) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(
                builder,
                1,
                1,
                ProfileGpuFrameStatus::Incomplete,
                false,
                7,
                _dropped_scope_count,
                0,
                "RHI drops"
            );
            add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "joint-root");
            add_scope(builder, 4, 1, 2, 99, 0, 2, 1, "joint-child-b");
            add_scope(builder, 6, 1, 3, 100, 0, 5, 2, "joint-child-c");
            add_scope(builder, 8, 1, 4, 101, 0, _last_child_local_order, 1, "joint-child-d");
            add_loss(builder, 3, 3, 1);
            add_loss(builder, 5, 5, 1);
            add_loss(builder, 7, 7, 1);
            builder.End();
            return builder;
        };

        SessionBuilder no_post_c_local_hole = make_joint_local_sequence_fixture(6, 0);
        expect_rejected(
            LoadChunks(no_post_c_local_hole.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "two virtual GPU parents reused one pre-C sequence/Loss slot"
        );

        SessionBuilder joint_control        = make_joint_local_sequence_fixture(7, 1);
        const Loaded   joint_control_loaded = LoadChunks(joint_control.bytes);
        Expect(
            joint_control_loaded.result.status == SessionLoadStatus::Complete &&
                joint_control_loaded.session.Valid(),
            "a virtual GPU parent in the post-C local/sequence cell was rejected"
        );

        const std::uint64_t exact_flow_edges = MinimumPassingTopologyFlowEdges(joint_control.bytes);
        Expect(
            exact_flow_edges == 22,
            "joint topology flow-edge count disagreed with the independent oracle (actual=" +
                std::to_string(exact_flow_edges) + ")"
        );
        SessionLoadOptions exact{};
        exact.limits.max_topology_flow_edges = exact_flow_edges;
        const Loaded exact_loaded            = LoadChunks(joint_control.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::Complete && exact_loaded.session.Valid(),
            "the exact joint topology flow-edge limit was rejected"
        );
        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_flow_edges;
        const Loaded limited = LoadChunks(joint_control.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyFlowEdges &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "joint topology flow edges escaped their resource limit or published a partial session"
        );
        SessionLoadOptions zero_edges{};
        zero_edges.limits.max_topology_flow_edges = 0;
        const Loaded task_limited                 = LoadChunks(joint_control.bytes, {}, zero_edges);
        Expect(
            task_limited.result.status == SessionLoadStatus::LimitExceeded &&
                task_limited.result.limit_kind == SessionLimitKind::TopologyFlowEdges &&
                !task_limited.result.HasUsableSession() && !task_limited.session.Valid(),
            "virtual GPU task allocation escaped the zero flow-edge limit"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Incomplete, false, 8, 2, 0, "optional empty cell");
        add_scope(builder, 2, 1, 1, 0, 0, 0, 0, "empty-cell-root");
        add_scope(builder, 4, 1, 2, 99, 0, 2, 1, "empty-cell-b");
        add_scope(builder, 6, 1, 3, 100, 0, 5, 2, "empty-cell-c");
        add_scope(builder, 7, 1, 4, 3, 0, 7, 3, "empty-cell-explicit-c-child");
        add_scope(builder, 9, 1, 5, 101, 0, 9, 1, "empty-cell-d");
        add_loss(builder, 3, 3, 1);
        add_loss(builder, 5, 5, 1);
        add_loss(builder, 8, 8, 1);
        builder.End();
        const Loaded loaded = LoadChunks(builder.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "an optional empty local/sequence cell rejected a task with a later valid cell"
        );
        const std::uint64_t exact_flow_edges = MinimumPassingTopologyFlowEdges(builder.bytes);
        Expect(
            exact_flow_edges == 22,
            "optional-cell topology flow-edge count disagreed with the independent oracle (actual=" +
                std::to_string(exact_flow_edges) + ")"
        );
        SessionLoadOptions exact{};
        exact.limits.max_topology_flow_edges = exact_flow_edges;
        const Loaded exact_loaded            = LoadChunks(builder.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::Complete && exact_loaded.session.Valid(),
            "the exact optional-cell topology flow-edge limit was rejected"
        );
        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_flow_edges;
        const Loaded limited = LoadChunks(builder.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyFlowEdges &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "a pruned optional cell polluted the exact flow-edge limit or published a partial session"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(
            builder,
            std::numeric_limits<std::uint64_t>::max() - 2,
            1,
            ProfileGpuFrameStatus::Incomplete,
            false,
            2,
            0,
            0,
            ""
        );
        add_scope(
            builder,
            std::numeric_limits<std::uint64_t>::max(),
            1,
            2,
            99,
            0,
            1,
            1,
            "forensic-upper-bound-joint-child"
        );

        SessionLoadOptions forensic{};
        forensic.allow_forensic_truncation = true;
        const Loaded loaded                = LoadChunks(builder.bytes, {}, forensic);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated && loaded.session.Valid(),
            "forensic joint topology rejected the UINT64_MAX-adjacent sequence hole"
        );

        const std::uint64_t exact_flow_edges = MinimumPassingTopologyFlowEdges(builder.bytes, forensic);
        Expect(
            exact_flow_edges == 6,
            "forensic joint topology flow-edge count disagreed with the independent oracle (actual=" +
                std::to_string(exact_flow_edges) + ")"
        );
        SessionLoadOptions exact             = forensic;
        exact.limits.max_topology_flow_edges = exact_flow_edges;
        const Loaded exact_loaded            = LoadChunks(builder.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                exact_loaded.session.Valid(),
            "the exact forensic joint topology flow-edge limit was rejected"
        );
        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_flow_edges;
        const Loaded limited = LoadChunks(builder.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyFlowEdges &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "forensic joint topology edges escaped their limit or published a partial session"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        AddCpuScope(builder, 1, 77, "cpu-only-over-budget-child", 10, 20, 3);
        AddCpuScope(builder, 5, 77, "cpu-only-over-budget-ancestor", 0, 30, 0);
        AddGpuFrame(builder, 6, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_loss(builder, 2, 2, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::CpuScopeParentMissing,
            "zero-demand GPU data changed a CPU-only over-budget diagnostic",
            "CPU hierarchy deficits exceed the noticed allocated-record loss budget"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddCpuScope(builder, 1, 77, "mixed-over-budget-cpu-child", 10, 20, 3);
        AddGpuFrame(builder, 2, 1, ProfileGpuFrameStatus::Incomplete, false, 2, 0, 0, "");
        add_scope(builder, 4, 1, 2, 99, 0, 1, 1, "mixed-over-budget-gpu-child");
        AddCpuScope(builder, 6, 77, "mixed-over-budget-cpu-ancestor", 0, 30, 0);
        add_loss(builder, 3, 3, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "a CPU deficit already over budget hid a simultaneous nontrusted GPU deficit",
            "CPU/GPU deficits exceed the noticed allocated-record loss budget"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddCpuScope(builder, 1, 77, "mixed-virtual-cpu-child", 10, 20, 2);
        AddGpuFrame(builder, 2, 1, ProfileGpuFrameStatus::Incomplete, false, 2, 0, 0, "");
        add_scope(builder, 4, 1, 2, 99, 0, 1, 1, "mixed-virtual-gpu-child");
        AddCpuScope(builder, 5, 77, "mixed-virtual-cpu-ancestor", 0, 30, 0);
        add_loss(builder, 3, 3, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "an insufficient mixed CPU/nontrusted-GPU Loss total was classified as a GPU-only failure",
            "CPU/GPU deficits exceed the noticed allocated-record loss budget"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddCpuScope(builder, 1, 77, "mixed-slot-cpu-child", 10, 20, 2);
        AddCpuScope(builder, 2, 88, "mixed-slot-occupied-record", 40, 50, 0);
        AddCpuScope(builder, 3, 77, "mixed-slot-cpu-ancestor", 0, 30, 0);
        AddGpuFrame(builder, 4, 1, ProfileGpuFrameStatus::Incomplete, false, 2, 0, 0, "");
        add_scope(builder, 6, 1, 2, 99, 0, 1, 1, "mixed-slot-gpu-child");
        add_loss(builder, 5, 7, 2);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "a CPU sequence-slot failure ignored a simultaneous nontrusted GPU topology demand",
            "missing CPU/GPU topology records cannot occupy distinct record-sequence holes"
        );
    }

    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        AddCpuScope(builder, 1, 77, "cpu-only-slot-child", 10, 20, 2);
        AddCpuScope(builder, 2, 88, "cpu-only-slot-occupied-record", 40, 50, 0);
        AddCpuScope(builder, 3, 77, "cpu-only-slot-ancestor", 0, 30, 0);
        AddGpuFrame(builder, 4, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        add_loss(builder, 5, 5, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::CpuScopeTopologyInvalid,
            "GPU records without a GPU topology demand changed a CPU-only sequence-slot failure",
            "missing CPU/GPU topology records cannot occupy distinct record-sequence holes"
        );
    }

    const auto make_cpu_gpu_shared_hole = [&](std::uint64_t _cpu_ancestor_sequence) {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddCpuScope(builder, 1, 77, "compressed-cpu-child", 10, 20, 2);
        AddGpuFrame(builder, 2, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        add_scope(builder, 4, 1, 2, 1, 0, 1, 1, "compressed-gpu-child");
        AddCpuScope(builder, _cpu_ancestor_sequence, 77, "compressed-cpu-ancestor", 0, 30, 0);
        return builder;
    };
    {
        SessionBuilder builder = make_cpu_gpu_shared_hole(6);
        add_loss(builder, 3, 3, 1);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "an insufficient mixed CPU/GPU Loss total was classified as a GPU-only failure",
            "CPU/GPU deficits exceed the noticed allocated-record loss budget"
        );
    }
    {
        SessionBuilder     builder = make_cpu_gpu_shared_hole(5);
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        expect_rejected(
            LoadChunks(builder.bytes, {}, options),
            SessionErrorCode::RecordSequenceInvalid,
            "forensic CPU/GPU topology demands reused one record-sequence hole"
        );

        SessionBuilder control        = make_cpu_gpu_shared_hole(6);
        const Loaded   control_loaded = LoadChunks(control.bytes, {}, options);
        Expect(
            control_loaded.result.status == SessionLoadStatus::ForensicTruncated &&
                control_loaded.session.Valid(),
            "forensic CPU/GPU topology with distinct holes was rejected"
        );
    }
    {
        SessionBuilder builder = make_cpu_gpu_shared_hole(7);
        add_loss(builder, 3, 8, 2);
        builder.End();
        expect_rejected(
            LoadChunks(builder.bytes),
            SessionErrorCode::RecordSequenceInvalid,
            "structural sequence capacity bypassed incompatible noticed-Loss intervals"
        );

        SessionBuilder control = make_cpu_gpu_shared_hole(7);
        add_loss(control, 3, 5, 2);
        control.End();
        const Loaded control_loaded = LoadChunks(control.bytes);
        Expect(
            control_loaded.result.status == SessionLoadStatus::Complete && control_loaded.session.Valid(),
            "compatible CPU/GPU structural and noticed-Loss sequence proofs were rejected"
        );

        const std::uint64_t exact_budget = MinimumPassingTopologyBudget(control.bytes);
        SessionLoadOptions  exact{};
        exact.limits.max_topology_work_items = exact_budget;
        const Loaded exact_loaded            = LoadChunks(control.bytes, {}, exact);
        Expect(
            exact_loaded.result.status == SessionLoadStatus::Complete && exact_loaded.session.Valid(),
            "the exact combined CPU/GPU sequence/Loss topology budget was rejected"
        );

        SessionLoadOptions short_by_one = exact;
        --short_by_one.limits.max_topology_work_items;
        const Loaded limited = LoadChunks(control.bytes, {}, short_by_one);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "combined CPU/GPU sequence/Loss work escaped the topology budget"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        AddGpuFrame(
            builder,
            std::numeric_limits<std::uint64_t>::max() - 1,
            1,
            ProfileGpuFrameStatus::Complete,
            true,
            1,
            0,
            0,
            ""
        );
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const Loaded loaded               = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::ForensicTruncated && loaded.session.Valid(),
            "a forensic Complete-frame tail did not consume the inclusive UINT64_MAX sequence slot"
        );

        SessionBuilder exhausted;
        exhausted.Begin();
        exhausted.Schema(Templates::GpuFrame());
        AddGpuFrame(
            exhausted,
            std::numeric_limits<std::uint64_t>::max(),
            1,
            ProfileGpuFrameStatus::Complete,
            true,
            1,
            0,
            0,
            ""
        );
        expect_rejected(
            LoadChunks(exhausted.bytes, {}, options),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "a Complete-frame tail escaped an exhausted record-sequence domain"
        );
    }
    {
        const auto make_upper_bound_loss_fixture = [&](std::uint64_t _loss_first) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            AddGpuFrame(
                builder,
                std::numeric_limits<std::uint64_t>::max() - 2,
                1,
                ProfileGpuFrameStatus::Complete,
                true,
                2,
                0,
                0,
                ""
            );
            add_loss(builder, _loss_first, std::numeric_limits<std::uint64_t>::max(), 2);
            builder.End();
            return builder;
        };

        SessionBuilder incompatible =
            make_upper_bound_loss_fixture(std::numeric_limits<std::uint64_t>::max() - 3);
        expect_rejected(
            LoadChunks(incompatible.bytes),
            SessionErrorCode::GpuFrameTotalsMismatch,
            "Loss below a Complete frame was reused for a UINT64_MAX tail demand"
        );

        SessionBuilder control = make_upper_bound_loss_fixture(std::numeric_limits<std::uint64_t>::max() - 1);
        const Loaded   loaded  = LoadChunks(control.bytes);
        Expect(
            loaded.result.status == SessionLoadStatus::Complete && loaded.session.Valid(),
            "a non-forensic Complete-frame tail failed Loss flow at UINT64_MAX"
        );
    }
}

void TestReaderPublicationAndAllocatorContracts() {
    const SessionBuilder    golden = MakeGoldenSession();
    ProfileSessionReader    reader;
    const PacketRange       begin = golden.ranges.front();
    const SessionLoadResult first =
        reader.Feed(std::span<const std::uint8_t>(golden.bytes).first(begin.size));
    Expect(first.status == SessionLoadStatus::Reading, "reader did not remain active after SessionBegin");

    const ProfileSession& unpublished        = reader.Session();
    const auto            unpublished_scopes = unpublished.CpuScopes();
    Expect(
        !unpublished.Valid() && unpublished_scopes.empty() && unpublished.Schemas().empty(),
        "reader exposed mutable session storage before Finish"
    );

    const SessionLoadResult rest =
        reader.Feed(std::span<const std::uint8_t>(golden.bytes).subspan(begin.size));
    Expect(rest.status == SessionLoadStatus::Reading, "reader published before EOF was established");
    ProfileSession early_take = reader.TakeSession();
    Expect(
        !reader.Session().Valid() && reader.Session().CpuScopes().empty() && !early_take.Valid(),
        "reader exposed or transferred the model after SessionEnd but before Finish"
    );
    const SessionLoadResult finished = reader.Finish();
    Expect(
        finished.status == SessionLoadStatus::Complete && reader.Session().Valid() &&
            unpublished_scopes.empty(),
        "reader did not atomically publish the immutable session at Finish"
    );

    bool malloc_n_overflow_threw = false;
    try {
        void* allocation = Memory::MallocN(std::numeric_limits<std::size_t>::max(), 2);
        Memory::Free(allocation);
    } catch (const std::bad_array_new_length&) {
        malloc_n_overflow_threw = true;
    }
    Expect(malloc_n_overflow_threw, "Memory::MallocN overflow did not throw std::bad_array_new_length");

    bool allocator_overflow_threw = false;
    try {
        MoerStlAllocator<std::uint64_t> allocator;
        [[maybe_unused]] auto*          allocation = allocator.allocate(allocator.max_size() + 1);
    } catch (const std::bad_alloc&) {
        allocator_overflow_threw = true;
    }
    Expect(
        allocator_overflow_threw, "MoerStlAllocator overflow did not follow the standard exception contract"
    );
}

void ExpectCancelledResult(const SessionLoadResult& _result, std::string_view _context) {
    Expect(
        _result.status == SessionLoadStatus::Cancelled && _result.error_code == SessionErrorCode::Cancelled &&
            _result.incomplete_reason == SessionIncompleteReason::None &&
            _result.limit_kind == SessionLimitKind::None && _result.codec_status == DecodeStatus::Ok &&
            _result.error_byte_offset == kInvalidSessionIndex &&
            _result.error_packet_index == kInvalidSessionIndex &&
            _result.incomplete_byte_offset == kInvalidSessionIndex &&
            _result.incomplete_packet_index == kInvalidSessionIndex && _result.IsTerminal() &&
            !_result.HasUsableSession(),
        _context
    );
}

void TestReaderCooperativeCancellation() {
    const SessionBuilder golden = MakeGoldenSession();

    {
        std::stop_source source;
        source.request_stop();
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        ProfileSessionReader reader({}, control);
        ExpectCancelledResult(reader.Result(), "pre-cancelled reader did not start terminal");
        ExpectCancelledResult(
            reader.Feed(golden.bytes), "pre-cancelled reader Feed did not preserve sticky cancellation"
        );
        ExpectCancelledResult(
            reader.Finish(), "pre-cancelled reader Finish did not preserve sticky cancellation"
        );
        Expect(
            !reader.Session().Valid() && !reader.TakeSession().Valid(),
            "pre-cancelled reader published a session"
        );
    }

    {
        std::stop_source         source;
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        ProfileSessionReader    reader({}, control);
        const PacketRange&      begin = golden.ranges.front();
        const SessionLoadResult fed =
            reader.Feed(std::span<const std::uint8_t>(golden.bytes).first(begin.size));
        Expect(
            fed.status == SessionLoadStatus::Reading && fed.packet_count == 1 &&
                fed.valid_prefix_bytes == begin.size,
            "reader cancellation fixture did not admit its complete prefix"
        );
        source.request_stop();
        const SessionLoadResult cancelled = reader.Feed({});
        ExpectCancelledResult(cancelled, "empty Feed did not observe cancellation between chunks");
        Expect(
            cancelled.packet_count == 1 && cancelled.valid_prefix_bytes == begin.size &&
                !reader.Session().Valid(),
            "between-chunk cancellation damaged the valid-prefix counters or published the model"
        );
        ExpectCancelledResult(reader.Finish(), "between-chunk cancellation was not sticky");
    }

    {
        const SessionLoadControl control{
            .max_work_items_before_cancel = static_cast<std::uint64_t>(golden.ranges.size()),
        };
        ProfileSessionReader    reader({}, control);
        const SessionLoadResult fed = reader.Feed(golden.bytes);
        Expect(
            fed.status == SessionLoadStatus::Reading && fed.packet_count == golden.ranges.size() &&
                fed.valid_prefix_bytes == golden.bytes.size(),
            "deterministic cancellation budget did not admit exactly N packet work items"
        );
        ExpectCancelledResult(
            reader.Feed({}), "deterministic cancellation budget did not stop at the next checkpoint"
        );
    }

    {
        const SessionLoadControl control{
            .max_work_items_before_cancel = static_cast<std::uint64_t>(golden.ranges.size()) + 1,
        };
        ProfileSessionReader    reader({}, control);
        const SessionLoadResult fed = reader.Feed(golden.bytes);
        Expect(
            fed.status == SessionLoadStatus::Reading && fed.packet_count == golden.ranges.size(),
            "Finish cancellation budget expired during Feed"
        );
        ExpectCancelledResult(reader.Finish(), "materialization budget did not cancel Finish");
        Expect(
            !reader.Session().Valid() && !reader.TakeSession().Valid(),
            "cancelled Finish published a partially materialized session"
        );
    }

    {
        SessionLoadOptions options{};
        options.allow_forensic_truncation = true;
        const PacketRange& session_end    = golden.ranges.back();
        const auto         prefix = std::span<const std::uint8_t>(golden.bytes).first(session_end.offset);
        const SessionLoadControl control{
            .max_work_items_before_cancel = static_cast<std::uint64_t>(golden.ranges.size()),
        };
        ProfileSessionReader    reader(options, control);
        const SessionLoadResult fed = reader.Feed(prefix);
        Expect(
            fed.status == SessionLoadStatus::Reading && fed.packet_count == golden.ranges.size() - 1,
            "forensic cancellation budget expired during Feed"
        );
        ExpectCancelledResult(
            reader.Finish(), "cancelled forensic materialization was incorrectly exposed as usable"
        );
        Expect(!reader.Session().Valid(), "cancelled forensic materialization published a session");
    }

    {
        std::stop_source         source;
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        ProfileSessionReader reader({}, control);
        const PacketRange&   begin = golden.ranges.front();
        Expect(
            reader.Feed(std::span<const std::uint8_t>(golden.bytes).first(begin.size)).status ==
                SessionLoadStatus::Reading,
            "reader move cancellation fixture did not admit SessionBegin"
        );
        ProfileSessionReader moved(std::move(reader));
        source.request_stop();
        ExpectCancelledResult(moved.Feed({}), "moved reader lost its stop-state connection");
        Expect(
            reader.Result().status == SessionLoadStatus::ResourceExhausted,
            "moved-from reader did not retain its documented empty state"
        );
    }

    {
        std::stop_source   source;
        SessionLoadOptions options{};
        options.limits.max_packets = 0;
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        ProfileSessionReader    reader(options, control);
        const PacketRange&      begin = golden.ranges.front();
        const SessionLoadResult limited =
            reader.Feed(std::span<const std::uint8_t>(golden.bytes).first(begin.size));
        Expect(
            limited.status == SessionLoadStatus::LimitExceeded &&
                limited.error_code == SessionErrorCode::LimitExceeded &&
                limited.limit_kind == SessionLimitKind::Packets,
            "sticky cancellation fixture did not reach LimitExceeded"
        );
        source.request_stop();
        Expect(
            reader.Feed({}).status == limited.status && reader.Finish().status == limited.status &&
                reader.Result().error_code == limited.error_code,
            "stop request overwrote an existing LimitExceeded terminal state"
        );
    }

    {
        std::vector<std::uint8_t> corrupt(
            golden.bytes.begin(),
            golden.bytes.begin() + static_cast<std::ptrdiff_t>(golden.ranges.front().size)
        );
        corrupt.back() ^= 0x80;
        std::stop_source         source;
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        ProfileSessionReader    reader({}, control);
        const SessionLoadResult rejected = reader.Feed(corrupt);
        Expect(
            rejected.status == SessionLoadStatus::CorruptData &&
                rejected.error_code == SessionErrorCode::CodecPacketInvalid,
            "sticky cancellation fixture did not reach CorruptData"
        );
        source.request_stop();
        Expect(
            reader.Feed({}).status == rejected.status && reader.Finish().status == rejected.status &&
                reader.Result().error_code == rejected.error_code,
            "stop request overwrote an existing CorruptData terminal state"
        );
    }
}

void TestConcurrentMidFinishCancellation() {
    constexpr std::uint64_t kScopeCount = 200'000;

    SessionBuilder builder(101);
    builder.Begin();
    builder.Schema(Templates::CpuScope());
    for (std::uint64_t index = 0; index < kScopeCount; ++index) {
        AddCpuScope(builder, index + 1, 77, "concurrent-cancel-root", index * 2, index * 2 + 1, 0);
    }
    builder.End();

    std::stop_source         stop_source;
    const SessionLoadControl control{
        .stop_token                  = stop_source.get_token(),
        .cancellation_check_interval = 64,
    };
    ProfileSessionReader    reader({}, control);
    const SessionLoadResult fed = reader.Feed(builder.bytes);
    Expect(
        fed.status == SessionLoadStatus::Reading && fed.packet_count == builder.ranges.size(),
        "concurrent cancellation fixture did not reach Finish"
    );

    std::atomic_bool  finish_started{false};
    std::atomic_bool  finish_done{false};
    SessionLoadResult finished{};
    std::thread       worker([&] {
        finish_started.store(true, std::memory_order_release);
        finished = reader.Finish();
        finish_done.store(true, std::memory_order_release);
    });

    while (!finish_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool request_was_mid_finish = !finish_done.load(std::memory_order_acquire);
    const auto cancel_begin           = std::chrono::steady_clock::now();
    stop_source.request_stop();
    worker.join();
    const auto cancel_latency = std::chrono::steady_clock::now() - cancel_begin;

    Expect(request_was_mid_finish, "concurrent cancellation fixture completed before request_stop");
    Expect(
        cancel_latency < std::chrono::seconds(5),
        "concurrent Finish cancellation exceeded the bounded-latency test threshold"
    );
    ExpectCancelledResult(finished, "concurrent request_stop did not cancel an active Finish");
    Expect(
        !reader.Session().Valid() && !reader.TakeSession().Valid(),
        "concurrent cancellation published a partially materialized session"
    );
}

void TestLimitsAndStickyFailure() {
    const SessionBuilder golden   = MakeGoldenSession();
    const Loaded         baseline = LoadChunks(golden.bytes);
    Expect(baseline.result.status == SessionLoadStatus::Complete, "limit baseline did not load");
    const auto expect_complete = [&](const SessionLoadOptions& _options, std::string_view _message) {
        Expect(LoadChunks(golden.bytes, {}, _options).result.status == SessionLoadStatus::Complete, _message);
    };
    {
        SessionLoadOptions options{};
        options.limits.max_packets = golden.ranges.size();
        expect_complete(options, "exact packet limit was rejected");
    }
    {
        SessionLoadOptions options{};
        options.limits.max_schemas = baseline.session.Schemas().size();
        expect_complete(options, "exact schema count limit was rejected");
        --options.limits.max_schemas;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Schemas,
            "schema count boundary was not enforced"
        );
    }
    {
        std::uint64_t schema_payload_bytes = 0;
        for (std::size_t index = 1; index <= 4; ++index) {
            schema_payload_bytes += golden.ranges[index].size - kPacketHeaderBytes;
        }
        SessionLoadOptions options{};
        options.limits.max_schema_bytes = schema_payload_bytes;
        expect_complete(options, "exact schema byte limit was rejected");
        --options.limits.max_schema_bytes;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::SchemaBytes,
            "schema byte boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_records = baseline.session.Summary().record_count;
        expect_complete(options, "exact record limit was rejected");
        --options.limits.max_records;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Records,
            "record boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_loss_notices = baseline.session.Losses().size();
        expect_complete(options, "exact Loss notice limit was rejected");
        --options.limits.max_loss_notices;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::LossNotices,
            "Loss notice boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_cpu_scopes = baseline.session.CpuScopes().size();
        expect_complete(options, "exact CPU scope limit was rejected");
        --options.limits.max_cpu_scopes;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::CpuScopes,
            "CPU scope boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_gpu_frames = baseline.session.GpuFrames().size();
        expect_complete(options, "exact GPU frame limit was rejected");
        --options.limits.max_gpu_frames;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::GpuFrames,
            "GPU frame boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_gpu_scopes = baseline.session.GpuScopes().size();
        expect_complete(options, "exact GPU scope limit was rejected");
        --options.limits.max_gpu_scopes;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::GpuScopes,
            "GPU scope boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_cpu_tracks = baseline.session.CpuTracks().size();
        expect_complete(options, "exact CPU track limit was rejected");
        --options.limits.max_cpu_tracks;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::CpuTracks,
            "CPU track boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_gpu_tracks = baseline.session.GpuTracks().size();
        expect_complete(options, "exact GPU track limit was rejected");
        --options.limits.max_gpu_tracks;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::GpuTracks,
            "GPU track boundary was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_gpu_domains = baseline.session.GpuDomains().size();
        expect_complete(options, "exact GPU domain limit was rejected");
    }
    {
        SessionLoadOptions options{};
        options.limits.max_logical_model_bytes = baseline.session.Summary().logical_model_bytes;
        expect_complete(options, "exact logical model byte limit was rejected");
        --options.limits.max_logical_model_bytes;
        const Loaded loaded = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::LogicalModelBytes,
            "logical model byte boundary was not enforced"
        );
    }
    {
        const SchemaDescriptor compact_schema{
            .name           = "a",
            .event_type     = "bb",
            .kind           = EventKind::Counter,
            .channel        = Channel::CpuThread,
            .schema_version = 1,
            .fields         = {{"c", FieldType::UInt64}},
        };
        SessionBuilder compact;
        compact.Begin();
        compact.Schema(compact_schema);
        compact.End();

        SessionLoadOptions options{};
        options.limits.max_unique_strings = 3;
        Expect(
            LoadChunks(compact.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact unique string limit was rejected"
        );
        options.limits.max_unique_strings = 2;
        Loaded loaded                     = LoadChunks(compact.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::UniqueStrings,
            "unique string boundary was not enforced"
        );

        options                         = {};
        options.limits.max_string_bytes = 4;
        Expect(
            LoadChunks(compact.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact cumulative string byte limit was rejected"
        );
        options.limits.max_string_bytes = 3;
        loaded                          = LoadChunks(compact.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::StringBytes,
            "cumulative string byte boundary was not enforced"
        );

        options                         = {};
        options.limits.codec.max_fields = 1;
        Expect(
            LoadChunks(compact.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact codec field limit was rejected"
        );
        options.limits.codec.max_fields = 0;
        loaded                          = LoadChunks(compact.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Codec,
            "codec field boundary was not enforced"
        );

        options                               = {};
        options.limits.codec.max_string_bytes = 2;
        Expect(
            LoadChunks(compact.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact codec string limit was rejected"
        );
        options.limits.codec.max_string_bytes = 1;
        loaded                                = LoadChunks(compact.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Codec,
            "codec string boundary was not enforced"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        AddCpuScope(builder, 2, 1, "parent", 0, 20, 0);
        AddCpuScope(builder, 1, 1, "child", 5, 10, 1);
        builder.End();

        SessionLoadOptions options{};
        options.limits.max_scope_depth = 1;
        Expect(
            LoadChunks(builder.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact scope depth limit was rejected"
        );
        options.limits.max_scope_depth = 0;
        const Loaded loaded            = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::ScopeDepth,
            "scope depth boundary was not enforced"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
        AddGpuScope(
            builder,
            2,
            1,
            1,
            0,
            0,
            0,
            0,
            0,
            0,
            "gpu-parent",
            ProfileGpuScopeStatus::Ready,
            10,
            30,
            64,
            1.0,
            20.0,
            10.0,
            0,
            ""
        );
        AddGpuScope(
            builder,
            3,
            1,
            2,
            1,
            0,
            1,
            0,
            0,
            0,
            "gpu-child",
            ProfileGpuScopeStatus::Ready,
            15,
            25,
            64,
            1.0,
            10.0,
            10.0,
            1,
            ""
        );
        builder.End();

        SessionLoadOptions options{};
        options.limits.max_scope_depth = 1;
        Expect(
            LoadChunks(builder.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact GPU scope depth limit was rejected"
        );
        options.limits.max_scope_depth = 0;
        const Loaded loaded            = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::ScopeDepth,
            "GPU scope depth boundary was not enforced"
        );
    }
    {
        constexpr std::uint64_t scope_count = 5;
        SessionBuilder          builder;
        builder.Begin();
        builder.Schema(Templates::CpuScope());
        for (std::uint64_t index = 0; index < scope_count; ++index) {
            AddCpuScope(builder, index + 1, 1, "oracle-root", index * 20, index * 20 + 10, 0);
        }
        builder.End();

        const std::uint64_t expected_budget =
            TopologySortWorkOracle(scope_count) * 2 + TopologyLinearWorkOracle(scope_count) * 4;
        Expect(
            MinimumPassingTopologyBudget(builder.bytes) == expected_budget,
            "isolated CPU indexing disagreed with the independent sort/linear work oracle"
        );
    }
    {
        const auto add_loss = [](SessionBuilder& _builder) {
            _builder.Loss({
                .first_sequence = 4,
                .last_sequence  = 5,
                .record_count   = 2,
                .value_bytes    = 16,
                .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
            });
        };
        const auto make_gpu_fixture = [&](bool _with_loss, bool _two_sources = true) {
            SessionBuilder builder;
            builder.Begin();
            builder.Schema(Templates::GpuFrame());
            builder.Schema(Templates::GpuScope());
            AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 2, 0, 0, "");
            AddGpuScope(
                builder,
                2,
                1,
                2,
                0,
                0,
                0,
                0,
                0,
                0,
                "topology-source-zero-root",
                ProfileGpuScopeStatus::Ready,
                10,
                20,
                64,
                1.0,
                10.0,
                10.0,
                0,
                ""
            );
            AddGpuScope(
                builder,
                3,
                1,
                4,
                0,
                _two_sources ? 1 : 0,
                _two_sources ? 0 : 1,
                0,
                0,
                0,
                "topology-source-one-root",
                ProfileGpuScopeStatus::Ready,
                30,
                40,
                64,
                1.0,
                10.0,
                10.0,
                0,
                ""
            );
            if (_with_loss) {
                add_loss(builder);
            }
            builder.End();
            return builder;
        };
        const auto make_no_gpu_fixture = [&](bool _with_loss) {
            SessionBuilder         builder;
            const SchemaDescriptor unknown = UnknownSchema();
            builder.Begin();
            builder.Schema(unknown);
            for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
                const std::array<FieldValueView, 2> values{
                    sequence,
                    std::string_view("topology-budget-control"),
                };
                builder.Record(unknown, sequence, values);
            }
            if (_with_loss) {
                add_loss(builder);
            }
            builder.End();
            return builder;
        };

        const SessionBuilder combined           = make_gpu_fixture(true);
        const SessionBuilder gpu_only           = make_gpu_fixture(false);
        const SessionBuilder one_source_gpu     = make_gpu_fixture(false, false);
        const SessionBuilder no_gpu             = make_no_gpu_fixture(false);
        const SessionBuilder no_gpu_with_loss   = make_no_gpu_fixture(true);
        const std::uint64_t  combined_budget    = MinimumPassingTopologyBudget(combined.bytes);
        const std::uint64_t  gpu_only_budget    = MinimumPassingTopologyBudget(gpu_only.bytes);
        const std::uint64_t  one_source_budget  = MinimumPassingTopologyBudget(one_source_gpu.bytes);
        const std::uint64_t  no_gpu_budget      = MinimumPassingTopologyBudget(no_gpu.bytes);
        const std::uint64_t  no_gpu_loss_budget = MinimumPassingTopologyBudget(no_gpu_with_loss.bytes);
        Expect(
            no_gpu_loss_budget >= no_gpu_budget,
            "Loss topology work unexpectedly reduced the minimum passing budget"
        );
        const std::uint64_t     loss_delta               = no_gpu_loss_budget - no_gpu_budget;
        constexpr std::uint64_t mandatory_sequence_count = 2;
        constexpr std::uint64_t emitted_record_count     = 3;
        const std::uint64_t     expected_loss_delta =
            TopologyLinearWorkOracle(1) + TopologyLinearWorkOracle(mandatory_sequence_count) +
            TopologySortWorkOracle(mandatory_sequence_count) +
            TopologyLinearWorkOracle(mandatory_sequence_count) * 2 +
            TopologyBinarySearchWorkOracle(emitted_record_count) * mandatory_sequence_count;
        Expect(loss_delta == expected_loss_delta, "Loss topology work disagreed with the independent oracle");

        constexpr std::uint64_t frame_count       = 1;
        constexpr std::uint64_t gpu_scope_count   = 2;
        constexpr std::uint64_t domain_count      = 1;
        constexpr std::uint64_t depth_tree_leaves = 2;
        constexpr std::uint64_t two_source_count  = 2;
        constexpr std::uint64_t one_source_count  = 1;
        const std::uint64_t     record_index_work =
            TopologySortWorkOracle(emitted_record_count) + TopologyLinearWorkOracle(emitted_record_count);
        const std::uint64_t domain_track_work =
            TopologySortWorkOracle(frame_count) + TopologyLinearWorkOracle(frame_count) +
            TopologyLinearWorkOracle(gpu_scope_count) + TopologySortWorkOracle(gpu_scope_count) +
            TopologyLinearWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(domain_count) +
            TopologyLinearWorkOracle(gpu_scope_count) +
            gpu_scope_count *
                (TopologyBinarySearchWorkOracle(frame_count) + TopologyBinarySearchWorkOracle(domain_count)) +
            TopologySortWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(gpu_scope_count) * 2;
        const auto gpu_topology_work = [&](std::uint64_t _source_count) {
            return TopologyLinearWorkOracle(frame_count) * 3 + TopologyLinearWorkOracle(gpu_scope_count) +
                   TopologySortWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(gpu_scope_count) +
                   TopologyLinearWorkOracle(gpu_scope_count) * 10 +
                   TopologyLinearWorkOracle(gpu_scope_count) + TopologySortWorkOracle(gpu_scope_count) +
                   TopologyLinearWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(gpu_scope_count) * 2 +
                   gpu_scope_count * TopologyBinarySearchWorkOracle(frame_count) +
                   TopologyLinearWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(gpu_scope_count) * 4 +
                   TopologyLinearWorkOracle(gpu_scope_count) * 3 +
                   TopologyLinearWorkOracle(depth_tree_leaves * 2) +
                   TopologyLinearWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(depth_tree_leaves) +
                   TopologyLinearWorkOracle(gpu_scope_count) * 3 + TopologyLinearWorkOracle(_source_count) +
                   TopologySortWorkOracle(_source_count) + TopologyLinearWorkOracle(_source_count) * 2 +
                   TopologyLinearWorkOracle(_source_count) + TopologySortWorkOracle(_source_count) +
                   TopologyLinearWorkOracle(gpu_scope_count) + TopologyLinearWorkOracle(frame_count) +
                   TopologyBinarySearchWorkOracle(frame_count) +
                   TopologyLinearWorkOracle(gpu_scope_count) * 2 + TopologyLinearWorkOracle(frame_count) +
                   TopologyLinearWorkOracle(frame_count) * 3;
        };
        const std::uint64_t expected_two_source_budget =
            record_index_work + domain_track_work + gpu_topology_work(two_source_count);
        const std::uint64_t expected_one_source_budget =
            record_index_work + domain_track_work + gpu_topology_work(one_source_count);
        Expect(
            gpu_only_budget == expected_two_source_budget && expected_two_source_budget == 124,
            "two-source GPU base indexing disagreed with the independent work oracle (actual=" +
                std::to_string(gpu_only_budget) + ", oracle=" + std::to_string(expected_two_source_budget) +
                ")"
        );
        Expect(
            one_source_budget == expected_one_source_budget && expected_one_source_budget == 116 &&
                gpu_only_budget - one_source_budget == 8,
            "GPU source-evidence indexing was not accumulated independently"
        );
        Expect(
            combined_budget == gpu_only_budget + loss_delta,
            "Loss and GPU topology work were not additive across the unified budget"
        );

        SessionLoadOptions options{};
        options.limits.max_topology_work_items = combined_budget;
        Expect(
            LoadChunks(combined.bytes, {}, options).result.status == SessionLoadStatus::Complete,
            "exact global GPU topology work-item limit was rejected"
        );
        --options.limits.max_topology_work_items;
        const Loaded loaded = LoadChunks(combined.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !loaded.result.HasUsableSession() && !loaded.session.Valid(),
            "GPU topology work-item limit was not accumulated across recording sources"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_input_bytes = golden.bytes.size();
        expect_complete(options, "exact input byte limit was rejected");
    }
    {
        SessionLoadOptions zero_budget{};
        zero_budget.limits.max_topology_work_items = 0;

        SessionBuilder empty;
        empty.Begin();
        empty.End();
        Expect(
            LoadChunks(empty.bytes, {}, zero_budget).result.status == SessionLoadStatus::Complete,
            "empty session consumed topology work budget"
        );

        SessionBuilder with_cpu;
        with_cpu.Begin();
        with_cpu.Schema(Templates::CpuScope());
        AddCpuScope(with_cpu, 1, 1, "zero-budget-root", 0, 10, 0);
        with_cpu.End();
        const Loaded cpu_limited = LoadChunks(with_cpu.bytes, {}, zero_budget);
        Expect(
            cpu_limited.result.status == SessionLoadStatus::LimitExceeded &&
                cpu_limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !cpu_limited.result.HasUsableSession() && !cpu_limited.session.Valid(),
            "non-empty CPU indexing escaped the zero topology budget or published partial state"
        );

        SessionBuilder         record_only;
        const SchemaDescriptor unknown = UnknownSchema();
        record_only.Begin();
        record_only.Schema(unknown);
        const std::array<FieldValueView, 2> unknown_values{
            std::uint64_t{1},
            std::string_view("gpu-index-control"),
        };
        record_only.Record(unknown, 1, unknown_values);
        record_only.End();

        SessionBuilder frame_only;
        frame_only.Begin();
        frame_only.Schema(Templates::GpuFrame());
        AddGpuFrame(frame_only, 1, 1, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");
        frame_only.End();

        SessionLoadOptions record_budget{};
        record_budget.limits.max_topology_work_items = 1;
        Expect(
            LoadChunks(record_only.bytes, {}, record_budget).result.status == SessionLoadStatus::Complete,
            "single record did not fit its independent record-index budget"
        );
        const Loaded gpu_limited = LoadChunks(frame_only.bytes, {}, record_budget);
        Expect(
            gpu_limited.result.status == SessionLoadStatus::LimitExceeded &&
                gpu_limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !gpu_limited.result.HasUsableSession() && !gpu_limited.session.Valid() &&
                MinimumPassingTopologyBudget(frame_only.bytes) == 13,
            "GPU frame indexing escaped its GPU-specific topology budget"
        );

        SessionBuilder with_loss;
        with_loss.Begin();
        with_loss.Loss({
            .first_sequence = 1,
            .last_sequence  = 1,
            .record_count   = 1,
            .value_bytes    = 8,
            .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
        });
        with_loss.End();
        const Loaded limited = LoadChunks(with_loss.bytes, {}, zero_budget);
        Expect(
            limited.result.status == SessionLoadStatus::LimitExceeded &&
                limited.result.limit_kind == SessionLimitKind::TopologyWorkItems &&
                !limited.result.HasUsableSession() && !limited.session.Valid(),
            "Loss sequence allocation escaped the zero topology budget or published partial state"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_input_bytes = golden.bytes.size() - 1;
        const Loaded loaded            = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::InputBytes,
            "input byte limit was not enforced"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();

        Moer::Array<std::uint8_t>       forged_header;
        const Moer::Array<std::uint8_t> empty_payload;
        Expect(
            WrapPacket(
                PacketType::Schema, builder.next_packet_index, empty_payload, builder.limits, forged_header
            ) == EncodeStatus::Ok &&
                forged_header.size() == kPacketHeaderBytes,
            "could not build the packet-size boundary fixture"
        );
        const auto write_u32 = [](Moer::Array<std::uint8_t>& _bytes, std::size_t _offset, std::uint32_t _value
                               ) {
            for (std::size_t byte = 0; byte < sizeof(_value); ++byte) {
                _bytes[_offset + byte] = static_cast<std::uint8_t>(_value >> (byte * 8));
            }
        };
        write_u32(forged_header, 12, std::numeric_limits<std::uint32_t>::max());
        const auto header_prefix = std::span<const std::uint8_t>(forged_header).first(28);
        write_u32(forged_header, 28, crc32_fast(header_prefix.data(), header_prefix.size()));
        builder.bytes.insert(builder.bytes.end(), forged_header.begin(), forged_header.end());

        SessionLoadOptions options{};
        options.limits.codec.max_packet_payload_bytes = std::numeric_limits<std::uint32_t>::max();
        options.limits.max_input_bytes                = builder.bytes.size();
        const Loaded loaded                           = LoadChunks(builder.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded,
            "an unaddressable or input-impossible packet wire size was not rejected before reserve"
        );
        if constexpr (sizeof(std::size_t) == sizeof(std::uint32_t)) {
            Expect(
                loaded.result.limit_kind == SessionLimitKind::Codec &&
                    loaded.result.codec_status == DecodeStatus::PayloadTooLarge,
                "32-bit packet-size overflow did not use the stable codec limit contract"
            );
        } else {
            Expect(
                loaded.result.limit_kind == SessionLimitKind::InputBytes,
                "declared packet size bypassed the configured input byte limit"
            );
        }
    }
    {
        SessionLoadOptions options{};
        options.limits.max_packets = 1;
        const Loaded loaded        = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Packets,
            "packet limit was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_records = 1;
        const Loaded loaded        = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::Records,
            "record limit was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_gpu_domains = 1;
        const Loaded loaded            = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::GpuDomains,
            "GPU domain limit was not enforced"
        );
    }
    {
        SessionLoadOptions options{};
        options.limits.max_logical_model_bytes = 1;
        const Loaded loaded                    = LoadChunks(golden.bytes, {}, options);
        Expect(
            loaded.result.status == SessionLoadStatus::LimitExceeded &&
                loaded.result.limit_kind == SessionLimitKind::LogicalModelBytes,
            "model byte limit was not enforced"
        );
    }
    {
        SessionBuilder builder;
        builder.Begin();
        builder.Schema(UnknownSchema());
        const PacketRange  schema = builder.ranges[1];
        SessionLoadOptions options{};
        options.limits.codec.max_packet_payload_bytes = 16;
        const std::size_t       header_end            = schema.offset + kPacketHeaderBytes;
        ProfileSessionReader    reader(options);
        const SessionLoadResult first =
            reader.Feed(std::span<const std::uint8_t>(builder.bytes).first(header_end));
        Expect(
            first.status == SessionLoadStatus::LimitExceeded && first.limit_kind == SessionLimitKind::Codec,
            "oversized declared payload was not rejected from its header"
        );
        const SessionLoadResult sticky = reader.Feed(builder.bytes);
        Expect(
            sticky.status == first.status && reader.Finish().status == first.status,
            "fatal reader state was not sticky"
        );
    }
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        static std::atomic<std::uint64_t> next{0};
        for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
            const auto                  tick = std::chrono::steady_clock::now().time_since_epoch().count();
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() /
                ("profile-consumer-" + std::to_string(tick) + "-" + std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create profile consumer temp directory");
    }

    ~ScopedTempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

void WriteBytes(const std::filesystem::path& _path, std::span<const std::uint8_t> _bytes) {
    std::ofstream stream(_path, std::ios::binary | std::ios::trunc);
    Expect(stream.is_open(), "could not create profile fixture file");
    stream.write(reinterpret_cast<const char*>(_bytes.data()), static_cast<std::streamsize>(_bytes.size()));
    Expect(stream.good(), "could not write profile fixture file");
}

void TestFileWrapperAndMoveOwnership() {
    const SessionBuilder        golden = MakeGoldenSession();
    ScopedTempDirectory         temporary;
    const std::filesystem::path valid_path   = temporary.path / "valid.mpds";
    const std::filesystem::path invalid_path = temporary.path / "invalid.mpds";
    WriteBytes(valid_path, golden.bytes);
    const std::array<std::uint8_t, 3> invalid{1, 2, 3};
    WriteBytes(invalid_path, invalid);

    ProfileSession          session;
    const SessionLoadResult valid_result = LoadProfileSessionFile(valid_path, {}, session);
    Expect(
        valid_result.status == SessionLoadStatus::Complete && session.Valid() &&
            session.Summary().generation == 7,
        "file wrapper did not load a valid session"
    );

    const SessionLoadResult invalid_result = LoadProfileSessionFile(invalid_path, {}, session);
    Expect(
        invalid_result.status == SessionLoadStatus::CorruptData && session.Valid() &&
            session.Summary().generation == 7,
        "failed file load modified the caller's valid session"
    );

    ProfileSession moved = std::move(session);
    Expect(
        moved.Valid() && !session.Valid() && moved.String(moved.CpuScopes().front().name) == "cpu-parent",
        "ProfileSession move did not preserve ownership"
    );

    ProfileSession          missing_output;
    const SessionLoadResult missing =
        LoadProfileSessionFile(temporary.path / "missing.mpds", {}, missing_output);
    Expect(
        missing.status == SessionLoadStatus::OpenFailed && !missing_output.Valid(),
        "missing file was not reported as OpenFailed"
    );

    SessionLoadOptions constrained{};
    constrained.limits.max_input_bytes = golden.bytes.size() - 1;
    const SessionLoadResult preflight  = LoadProfileSessionFile(valid_path, constrained, moved);
    Expect(
        preflight.status == SessionLoadStatus::LimitExceeded &&
            preflight.limit_kind == SessionLimitKind::InputBytes &&
            preflight.input_bytes == golden.bytes.size() && moved.Valid() && moved.Summary().generation == 7,
        "file-size preflight did not enforce the exact input limit atomically"
    );

    {
        std::stop_source source;
        source.request_stop();
        const SessionLoadControl control{
            .stop_token = source.get_token(),
        };
        const SessionLoadResult cancelled = LoadProfileSessionFile(valid_path, {}, moved, control);
        ExpectCancelledResult(cancelled, "file wrapper did not report pre-cancellation");
        Expect(
            moved.Valid() && moved.Summary().generation == 7,
            "pre-cancelled file load replaced the caller's valid output"
        );
    }

    {
        const SessionLoadControl control{
            .max_work_items_before_cancel = static_cast<std::uint64_t>(golden.ranges.size()) + 1,
        };
        const SessionLoadResult cancelled = LoadProfileSessionFile(valid_path, {}, moved, control);
        ExpectCancelledResult(cancelled, "file wrapper did not cancel Finish materialization");
        Expect(
            moved.Valid() && moved.Summary().generation == 7,
            "Finish-cancelled file load replaced the caller's valid output"
        );
    }

    SessionBuilder transient_fixture(99);
    transient_fixture.Begin();
    const SchemaDescriptor unknown = UnknownSchema();
    transient_fixture.Schema(unknown);
    const std::array<FieldValueView, 2> first_values{
        std::uint64_t{1},
        std::string_view("first"),
    };
    const std::array<FieldValueView, 2> second_values{
        std::uint64_t{2},
        std::string_view("second"),
    };
    transient_fixture.Record(unknown, 1, first_values);
    transient_fixture.Record(unknown, 2, second_values);
    transient_fixture.End();
    const std::filesystem::path transient_path = temporary.path / "transient.mpds";
    WriteBytes(transient_path, transient_fixture.bytes);

    SessionLoadOptions transient_options{};
    transient_options.limits.max_transient_materialization_bytes = sizeof(std::uint64_t) * 2;
    const SessionLoadControl single_item_runs{
        .cancellation_check_interval = 1,
    };
    ProfileSession          transient_session;
    const SessionLoadResult exact_transient =
        LoadProfileSessionFile(transient_path, transient_options, transient_session, single_item_runs);
    Expect(
        exact_transient.status == SessionLoadStatus::Complete && transient_session.Valid() &&
            transient_session.Summary().generation == 99,
        "exact transient materialization byte limit was rejected"
    );

    --transient_options.limits.max_transient_materialization_bytes;
    const SessionLoadResult short_transient =
        LoadProfileSessionFile(transient_path, transient_options, moved, single_item_runs);
    Expect(
        short_transient.status == SessionLoadStatus::LimitExceeded &&
            short_transient.error_code == SessionErrorCode::LimitExceeded &&
            short_transient.limit_kind == SessionLimitKind::TransientMaterializationBytes && moved.Valid() &&
            moved.Summary().generation == 7,
        "transient materialization byte limit was not enforced atomically"
    );
}

void TestProducerToConsumerUnnotifiedDrop() {
    [[maybe_unused]] const ShutdownResult initial_shutdown = Shutdown();
    ScopedTempDirectory                   temporary;
    RuntimeConfig                         config{};
    config.output_path      = temporary.path / "generation-a.mpd";
    config.replace_existing = true;

    Expect(Start(config) == StartResult::Started, "producer integration could not start generation A");
    const SchemaDescriptor   unknown      = UnknownSchema();
    const SchemaRegistration registration = RegisterSchema(unknown);
    Expect(
        registration.status == SchemaStatus::Registered,
        "producer integration could not register generation A schema"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "producer integration could not close generation A");

    config.output_path = temporary.path / "generation-b.mpd";
    Expect(Start(config) == StartResult::Started, "producer integration could not start generation B");
    const std::array<FieldValueView, 2> stale_values{
        std::uint64_t{1},
        std::string_view("stale"),
    };
    Expect(
        Emit(registration.handle, stale_values) == EmitStatus::InvalidHandle,
        "producer integration did not exercise a stale-generation drop"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "producer integration could not close generation B");

    ProfileSession          session;
    const SessionLoadResult loaded = LoadProfileSessionFile(config.output_path, {}, session);
    Expect(
        loaded.status == SessionLoadStatus::Complete && session.Valid() &&
            session.Summary().session_end_records_dropped == 1 && session.Summary().lost_record_count == 0 &&
            session.Summary().unnotified_drop_count == 1,
        "consumer rejected a real producer dump containing an unnotified stale-handle drop"
    );

    config.output_path = temporary.path / "generation-c.mpd";
    Expect(Start(config) == StartResult::Started, "producer integration could not start generation C");
    const SchemaRegistration current_registration = RegisterSchema(Templates::CpuScope());
    Expect(
        current_registration.status == SchemaStatus::Registered,
        "producer integration could not register generation C schema"
    );
    const std::array<FieldValueView, 1> invalid_values{
        std::uint64_t{1},
    };
    Expect(
        Emit(current_registration.handle, invalid_values) == EmitStatus::ValueCountMismatch,
        "producer integration did not reserve a sequence for a rejected value set"
    );
    const std::array<FieldValueView, 5> valid_values{
        std::uint64_t{7},
        std::string_view("sequence-two"),
        std::uint64_t{10},
        std::uint64_t{20},
        std::uint32_t{0},
    };
    Expect(
        Emit(current_registration.handle, valid_values) == EmitStatus::Accepted,
        "producer integration could not emit the post-rejection record"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "producer integration could not close generation C");

    ProfileSession          sparse_session;
    const SessionLoadResult sparse_loaded = LoadProfileSessionFile(config.output_path, {}, sparse_session);
    Expect(
        sparse_loaded.status == SessionLoadStatus::Complete && sparse_session.Valid() &&
            sparse_session.Summary().record_count == 1 && sparse_session.CpuScopes().size() == 1 &&
            sparse_session.CpuScopes().front().sequence == 2 &&
            sparse_session.Summary().session_end_records_dropped == 0,
        "consumer did not preserve the post-rejection sequence gap without inventing a v3 frontier"
    );
}

} // namespace

int main() {
    try {
        TestGoldenSessionAndArbitraryChunking();
        TestEnvelopeSchemaAndSequenceContracts();
        TestChecksumAndForensicTruncation();
        TestSemanticTopologyAndLossRecovery();
        TestGpuProducerSequenceProofs();
        TestReaderPublicationAndAllocatorContracts();
        TestReaderCooperativeCancellation();
        TestConcurrentMidFinishCancellation();
        TestLimitsAndStickyFailure();
        TestFileWrapperAndMoveOwnership();
        TestProducerToConsumerUnnotifiedDrop();
        std::cout << "ProfileDump consumer contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProfileDump consumer contract failed: " << error.what() << '\n';
        return 1;
    }
}
