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
#include <vector>

namespace {

using namespace Moer::ProfileDump;

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
    const SessionLoadOptions&     _options = {}
) {
    ProfileSessionReader reader(_options);
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

SessionBuilder MakeGoldenSession() {
    SessionBuilder         builder;
    const SchemaDescriptor unknown = UnknownSchema();
    builder.Begin();
    builder.Schema(Templates::CpuScope());
    builder.Schema(Templates::GpuFrame());
    builder.Schema(Templates::GpuScope());
    builder.Schema(unknown);

    // Record sequence and packet order are deliberately unrelated. CPU child
    // and GPU child also precede their parents in the file.
    AddCpuScope(builder, 4, 11, "cpu-child", 120, 150, 1);
    AddCpuScope(builder, 7, 11, "cpu-parent", 100, 300, 0);
    AddCpuScope(builder, 2, 22, "cpu-other", 50, 60, 0);

    AddGpuFrame(builder, 10, 100, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
    AddGpuFrame(builder, 12, 101, ProfileGpuFrameStatus::Invalid, false, 1, 0, 1, "query failed");
    AddGpuScope(
        builder,
        8,
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
        14,
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
        16,
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
        18,
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
        20,
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
    builder.Record(unknown, 22, unknown_values);

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
    Expect(_loaded.result.status == SessionLoadStatus::Complete, "golden session did not complete");
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
            summary.incomplete_gpu_frame_count == 0 && summary.ready_gpu_scope_count == 4 &&
            summary.error_gpu_scope_count == 1,
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
            complete_frame->error_scope_count == 0 && invalid_frame != _loaded.session.GpuFrames().end() &&
            invalid_frame->status == ProfileGpuFrameStatus::Invalid && !invalid_frame->valid &&
            invalid_frame->admitted_scope_count == 1 && invalid_frame->scope_count == 1 &&
            invalid_frame->error_scope_count == 1 &&
            _loaded.session.String(invalid_frame->reason) == "query failed",
        "GPU frame counters or reason changed while materializing the session"
    );
    const GpuTimestampDomain& shared_domain = _loaded.session.GpuDomains()[graphics.domain_index];
    Expect(
        shared_domain.logical_queue_mask == (ProfileLogicalQueueBit(ProfileLogicalQueue::Graphics) |
                                             ProfileLogicalQueueBit(ProfileLogicalQueue::Compute)) &&
            shared_domain.valid_bits == 64 && shared_domain.tick_period_ns == 1.0 &&
            shared_domain.ready_scope_count == 3,
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
        builder.Record(unknown, 9, values);
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
            2,
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
        builder.Schema(Templates::GpuFrame());
        builder.Schema(Templates::GpuScope());
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
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
        AddGpuFrame(builder, 1, 1, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
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
                loaded.session.Summary().orphan_gpu_scope_count == 1,
            "loss-tolerant GPU orphan was not retained"
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
            loaded.result.status == SessionLoadStatus::Complete &&
                loaded.session.Summary().unnotified_drop_count == 1,
            "unnotified SessionEnd drops did not explain an undersized GPU frame"
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
        AddCpuScope(builder, 1, 1, "parent", 0, 20, 0);
        AddCpuScope(builder, 2, 1, "child", 5, 10, 1);
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
        SessionLoadOptions options{};
        options.limits.max_input_bytes = golden.bytes.size();
        expect_complete(options, "exact input byte limit was rejected");
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
}

} // namespace

int main() {
    try {
        TestGoldenSessionAndArbitraryChunking();
        TestEnvelopeSchemaAndSequenceContracts();
        TestChecksumAndForensicTruncation();
        TestSemanticTopologyAndLossRecovery();
        TestReaderPublicationAndAllocatorContracts();
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
