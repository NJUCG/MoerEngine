#include "profile_consumer/ProfileSession.h"

#include "profile/ProfileDumpTemplates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <queue>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Moer::ProfileDump {

namespace {

constexpr std::size_t kReadBlockBytes = 256 * 1024;

template<typename T>
[[nodiscard]] bool AddOverflow(T _left, T _right, T& _result) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if (_right > std::numeric_limits<T>::max() - _left) {
        return true;
    }
    _result = _left + _right;
    return false;
}

[[nodiscard]] std::uint32_t
ReadU32LittleEndian(std::span<const std::uint8_t> _bytes, std::size_t _offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint32_t>(_bytes[_offset + byte]) << (byte * 8);
    }
    return value;
}

[[nodiscard]] std::uint64_t
ReadU64LittleEndian(std::span<const std::uint8_t> _bytes, std::size_t _offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(_bytes[_offset + byte]) << (byte * 8);
    }
    return value;
}

[[nodiscard]] bool IsFiniteNonnegative(double _value) noexcept {
    return std::isfinite(_value) && _value >= 0.0;
}

[[nodiscard]] bool DurationContains(double _outer, double _inner) noexcept {
    const double tolerance = std::max(1e-9, std::abs(_outer) * 1e-9);
    return _inner <= _outer + tolerance;
}

[[nodiscard]] std::uint64_t TimestampMask(std::uint32_t _valid_bits) noexcept {
    return _valid_bits == 64 ? std::numeric_limits<std::uint64_t>::max() :
                               (std::uint64_t{1} << _valid_bits) - 1;
}

[[nodiscard]] std::uint64_t
TimestampDelta(std::uint64_t _begin_tick, std::uint64_t _end_tick, std::uint32_t _valid_bits) noexcept {
    return (_end_tick - _begin_tick) & TimestampMask(_valid_bits);
}

[[nodiscard]] bool NearlyEqualDuration(double _left, double _right) noexcept {
    if (!std::isfinite(_left) || !std::isfinite(_right)) {
        return false;
    }
    const double tolerance = std::max(1.0e-6, std::max(std::abs(_left), std::abs(_right)) * 1.0e-9);
    return std::abs(_left - _right) <= tolerance;
}

[[nodiscard]] bool CanBridgeSerialTimestampGap(
    std::uint64_t _duration,
    std::uint64_t _begin_offset,
    std::uint32_t _valid_bits,
    std::uint64_t _missing_local_orders
) noexcept {
    const std::uint64_t half_range =
        _valid_bits == 64 ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << (_valid_bits - 1));
    const std::uint64_t maximum_step = half_range - 1;
    if (_duration > maximum_step) {
        return false;
    }

    std::uint64_t step_count = 0;
    if (AddOverflow(_missing_local_orders, std::uint64_t{1}, step_count)) {
        return true;
    }
    if (maximum_step == 0) {
        return _begin_offset == 0 && _duration == 0;
    }

    if (_valid_bits != 64) {
        const std::uint64_t modulus  = std::uint64_t{1} << _valid_bits;
        const std::uint64_t distance = _begin_offset >= _duration ? _begin_offset : _begin_offset + modulus;
        const std::uint64_t required_steps =
            distance / maximum_step + static_cast<std::uint64_t>(distance % maximum_step != 0);
        return required_steps <= step_count;
    }

    std::uint64_t required_steps = 0;
    if (_begin_offset >= _duration) {
        required_steps =
            _begin_offset / maximum_step + static_cast<std::uint64_t>(_begin_offset % maximum_step != 0);
    } else if (_begin_offset <= maximum_step - 2) {
        required_steps = 3;
    } else if (_begin_offset <= std::numeric_limits<std::uint64_t>::max() - 3) {
        required_steps = 4;
    } else {
        required_steps = 5;
    }
    return required_steps <= step_count;
}

struct SerialTick {
    std::uint64_t epoch{0};
    std::uint64_t tick{0};

    [[nodiscard]] bool operator<(const SerialTick& _right) const noexcept {
        return std::tie(epoch, tick) < std::tie(_right.epoch, _right.tick);
    }
};

[[nodiscard]] bool SerialTickLessEqual(const SerialTick& _left, const SerialTick& _right) noexcept {
    return !(_right < _left);
}

[[nodiscard]] bool AddSerialTickDelta(
    SerialTick    _base,
    std::uint64_t _delta,
    std::uint32_t _valid_bits,
    SerialTick&   _result
) noexcept {
    if (_valid_bits == 64) {
        const std::uint64_t tick  = _base.tick + _delta;
        const std::uint64_t carry = static_cast<std::uint64_t>(tick < _base.tick);
        if (carry != 0 && _base.epoch == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        _result = {
            .epoch = _base.epoch + carry,
            .tick  = tick,
        };
        return true;
    }

    const std::uint64_t modulus = std::uint64_t{1} << _valid_bits;
    const std::uint64_t sum     = _base.tick + _delta;
    const std::uint64_t carry   = static_cast<std::uint64_t>(sum >= modulus);
    if (carry != 0 && _base.epoch == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    _result = {
        .epoch = _base.epoch + carry,
        .tick  = carry != 0 ? sum - modulus : sum,
    };
    return true;
}

[[nodiscard]] bool SubtractSerialTickDelta(
    SerialTick    _base,
    std::uint64_t _delta,
    std::uint32_t _valid_bits,
    SerialTick&   _result
) noexcept {
    if (_base.tick >= _delta) {
        _result = {
            .epoch = _base.epoch,
            .tick  = _base.tick - _delta,
        };
        return true;
    }
    if (_base.epoch == 0) {
        return false;
    }
    if (_valid_bits == 64) {
        _result = {
            .epoch = _base.epoch - 1,
            .tick  = _base.tick - _delta,
        };
        return true;
    }
    const std::uint64_t modulus = std::uint64_t{1} << _valid_bits;
    _result                     = {
                            .epoch = _base.epoch - 1,
                            .tick  = modulus - (_delta - _base.tick),
    };
    return true;
}

[[nodiscard]] bool NextSerialTickOccurrence(
    std::uint64_t     _tick,
    std::uint32_t     _valid_bits,
    const SerialTick& _lower_bound,
    SerialTick&       _result
) noexcept {
    _result = {
        .epoch = _lower_bound.epoch,
        .tick  = _tick & TimestampMask(_valid_bits),
    };
    if (_result < _lower_bound) {
        if (_result.epoch == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++_result.epoch;
    }
    return true;
}

[[nodiscard]] KnownProfileSchema IdentifyKnownSchema(const SchemaDescriptor& _schema) {
    if (_schema == Templates::CpuScope()) {
        return KnownProfileSchema::CpuScopeV1;
    }
    if (_schema == Templates::GpuFrame()) {
        return KnownProfileSchema::GpuFrameV1;
    }
    if (_schema == Templates::GpuScope()) {
        return KnownProfileSchema::GpuScopeV2;
    }
    return KnownProfileSchema::Unknown;
}

template<typename T>
[[nodiscard]] const T* RecordField(const DecodedRecord& _record, std::size_t _index) noexcept {
    if (_index >= _record.values.size()) {
        return nullptr;
    }
    return std::get_if<T>(&_record.values[_index]);
}

[[nodiscard]] SessionLoadResult ResourceExhaustedResult() noexcept {
    SessionLoadResult result{};
    result.status     = SessionLoadStatus::ResourceExhausted;
    result.error_code = SessionErrorCode::ResourceAllocationFailed;
    return result;
}

} // namespace

struct ProfileSession::Impl {
    bool                  valid{false};
    ProfileSessionSummary summary{};

    std::vector<ProfileSchemaInfo>      schemas{};
    std::vector<ProfileSchemaFieldInfo> schema_fields{};
    std::vector<ProfileLossRecord>      losses{};
    std::vector<CpuScopeRecord>         cpu_scopes{};
    std::vector<CpuTrack>               cpu_tracks{};
    std::vector<GpuFrameRecord>         gpu_frames{};
    std::vector<GpuScopeRecord>         gpu_scopes{};
    std::vector<GpuTrack>               gpu_tracks{};
    std::vector<GpuTimestampDomain>     gpu_domains{};
    std::deque<std::string>             strings{};
};

ProfileSession::~ProfileSession() {
    delete impl_;
}

ProfileSession::ProfileSession(ProfileSession&& _other) noexcept :
    impl_(std::exchange(_other.impl_, nullptr)) {}

ProfileSession& ProfileSession::operator=(ProfileSession&& _other) noexcept {
    if (this != &_other) {
        delete impl_;
        impl_ = std::exchange(_other.impl_, nullptr);
    }
    return *this;
}

bool ProfileSession::Valid() const noexcept {
    return impl_ != nullptr && impl_->valid;
}

const ProfileSessionSummary& ProfileSession::Summary() const noexcept {
    static const ProfileSessionSummary empty{};
    return Valid() ? impl_->summary : empty;
}

std::span<const ProfileSchemaInfo> ProfileSession::Schemas() const noexcept {
    return Valid() ? std::span<const ProfileSchemaInfo>(impl_->schemas) :
                     std::span<const ProfileSchemaInfo>{};
}

std::span<const ProfileSchemaFieldInfo> ProfileSession::SchemaFields() const noexcept {
    return Valid() ? std::span<const ProfileSchemaFieldInfo>(impl_->schema_fields) :
                     std::span<const ProfileSchemaFieldInfo>{};
}

std::span<const ProfileLossRecord> ProfileSession::Losses() const noexcept {
    return Valid() ? std::span<const ProfileLossRecord>(impl_->losses) : std::span<const ProfileLossRecord>{};
}

std::span<const CpuScopeRecord> ProfileSession::CpuScopes() const noexcept {
    return Valid() ? std::span<const CpuScopeRecord>(impl_->cpu_scopes) : std::span<const CpuScopeRecord>{};
}

std::span<const CpuTrack> ProfileSession::CpuTracks() const noexcept {
    return Valid() ? std::span<const CpuTrack>(impl_->cpu_tracks) : std::span<const CpuTrack>{};
}

std::span<const GpuFrameRecord> ProfileSession::GpuFrames() const noexcept {
    return Valid() ? std::span<const GpuFrameRecord>(impl_->gpu_frames) : std::span<const GpuFrameRecord>{};
}

std::span<const GpuScopeRecord> ProfileSession::GpuScopes() const noexcept {
    return Valid() ? std::span<const GpuScopeRecord>(impl_->gpu_scopes) : std::span<const GpuScopeRecord>{};
}

std::span<const GpuTrack> ProfileSession::GpuTracks() const noexcept {
    return Valid() ? std::span<const GpuTrack>(impl_->gpu_tracks) : std::span<const GpuTrack>{};
}

std::span<const GpuTimestampDomain> ProfileSession::GpuDomains() const noexcept {
    return Valid() ? std::span<const GpuTimestampDomain>(impl_->gpu_domains) :
                     std::span<const GpuTimestampDomain>{};
}

std::string_view ProfileSession::String(SessionStringId _id) const noexcept {
    if (!Valid() || _id == kInvalidSessionString || static_cast<std::size_t>(_id) >= impl_->strings.size()) {
        return {};
    }
    return impl_->strings[_id];
}

struct ProfileSessionReader::Impl {
    struct SchemaEntry {
        SchemaDescriptor   descriptor{};
        KnownProfileSchema known_schema{KnownProfileSchema::Unknown};
        std::size_t        info_index{0};
    };

    struct DomainKey {
        std::uint32_t native_queue_id{0};
        std::uint32_t family_id{0};

        friend bool operator==(const DomainKey&, const DomainKey&) = default;
        friend bool operator<(const DomainKey& _left, const DomainKey& _right) noexcept {
            if (_left.native_queue_id != _right.native_queue_id) {
                return _left.native_queue_id < _right.native_queue_id;
            }
            return _left.family_id < _right.family_id;
        }
    };

    struct ScopeLookup {
        std::uint64_t frame_id{0};
        std::uint64_t scope_id{0};
        std::uint64_t scope_index{0};

        friend bool operator<(const ScopeLookup& _left, const ScopeLookup& _right) noexcept {
            if (_left.frame_id != _right.frame_id) {
                return _left.frame_id < _right.frame_id;
            }
            if (_left.scope_id != _right.scope_id) {
                return _left.scope_id < _right.scope_id;
            }
            return _left.scope_index < _right.scope_index;
        }
    };

    struct RecordSequenceDemand {
        std::uint64_t release_sequence{0};
        std::uint64_t deadline_sequence{0};
        std::uint64_t count{0};
    };

    explicit Impl(const SessionLoadOptions& _options) noexcept : options(_options) {
        session.impl_ = new (std::nothrow) ProfileSession::Impl();
        if (session.impl_ == nullptr) {
            result.status     = SessionLoadStatus::ResourceExhausted;
            result.error_code = SessionErrorCode::ResourceAllocationFailed;
            diagnostic        = "profile session model allocation failed";
        }
    }

    [[nodiscard]] bool IsReading() const noexcept {
        return result.status == SessionLoadStatus::Reading;
    }

    void Fail(
        SessionLoadStatus _status,
        SessionErrorCode  _error_code,
        const char*       _diagnostic,
        DecodeStatus      _codec_status = DecodeStatus::Ok,
        SessionLimitKind  _limit_kind   = SessionLimitKind::None,
        std::uint64_t     _byte_offset  = kInvalidSessionIndex
    ) noexcept {
        if (!IsReading()) {
            return;
        }
        result.status       = _status;
        result.error_code   = _error_code;
        result.codec_status = _codec_status;
        result.limit_kind   = _limit_kind;
        result.error_byte_offset =
            _byte_offset == kInvalidSessionIndex ? result.valid_prefix_bytes : _byte_offset;
        result.error_packet_index = expected_packet_index;
        diagnostic                = _diagnostic;
        if (session.impl_ != nullptr) {
            session.impl_->valid = false;
        }
    }

    [[nodiscard]] bool ChargeModel(std::uint64_t _bytes) noexcept {
        std::uint64_t next = 0;
        if (AddOverflow(session.impl_->summary.logical_model_bytes, _bytes, next) ||
            next > options.limits.max_logical_model_bytes) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile session model byte limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::LogicalModelBytes
            );
            return false;
        }
        session.impl_->summary.logical_model_bytes = next;
        return true;
    }

    [[nodiscard]] bool CheckCountLimit(
        std::uint64_t    _current,
        std::uint64_t    _maximum,
        SessionLimitKind _kind,
        const char*      _diagnostic
    ) noexcept {
        if (_current >= _maximum) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                _diagnostic,
                DecodeStatus::LimitExceeded,
                _kind
            );
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ChargeTopologyWork(std::uint64_t _count) noexcept {
        std::uint64_t next_topology_work_items = 0;
        if (AddOverflow(topology_work_items, _count, next_topology_work_items) ||
            next_topology_work_items > options.limits.max_topology_work_items) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile topology work-item limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        topology_work_items = next_topology_work_items;
        return true;
    }

    [[nodiscard]] bool InternString(std::string_view _value, SessionStringId& _id) {
        const auto existing = interned_strings.find(_value);
        if (existing != interned_strings.end()) {
            _id = existing->second;
            return true;
        }

        if (session.impl_->strings.size() >= options.limits.max_unique_strings ||
            session.impl_->strings.size() >=
                static_cast<std::size_t>(std::numeric_limits<SessionStringId>::max())) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile session unique string limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::UniqueStrings
            );
            return false;
        }

        std::uint64_t next_string_bytes = 0;
        if (AddOverflow(string_bytes, static_cast<std::uint64_t>(_value.size()), next_string_bytes) ||
            next_string_bytes > options.limits.max_string_bytes) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile session string byte limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::StringBytes
            );
            return false;
        }
        if (!ChargeModel(sizeof(std::string) + static_cast<std::uint64_t>(_value.size()))) {
            return false;
        }

        session.impl_->strings.emplace_back(_value);
        const SessionStringId  id     = static_cast<SessionStringId>(session.impl_->strings.size() - 1);
        const std::string_view stored = session.impl_->strings.back();
        interned_strings.emplace(stored, id);
        string_bytes = next_string_bytes;
        _id          = id;
        return true;
    }

    void FailCodec(DecodeStatus _status, SessionErrorCode _error_code, const char* _context) noexcept {
        SessionLoadStatus status = SessionLoadStatus::CorruptData;
        SessionLimitKind  limit  = SessionLimitKind::None;
        if (_status == DecodeStatus::UnsupportedVersion) {
            status = SessionLoadStatus::UnsupportedVersion;
        } else if (_status == DecodeStatus::PayloadTooLarge || _status == DecodeStatus::LimitExceeded) {
            status = SessionLoadStatus::LimitExceeded;
            limit  = SessionLimitKind::Codec;
        } else if (_status == DecodeStatus::UnknownSchema) {
            status = SessionLoadStatus::ProtocolViolation;
        }
        Fail(status, _error_code, _context, _status, limit);
    }

    [[nodiscard]] bool MaterializeCpuScope(const DecodedRecord& _record) {
        const auto* thread_id = RecordField<std::uint64_t>(_record, 0);
        const auto* name      = RecordField<ProfileString>(_record, 1);
        const auto* begin_ns  = RecordField<std::uint64_t>(_record, 2);
        const auto* end_ns    = RecordField<std::uint64_t>(_record, 3);
        const auto* depth     = RecordField<std::uint32_t>(_record, 4);
        if (thread_id == nullptr || name == nullptr || begin_ns == nullptr || end_ns == nullptr ||
            depth == nullptr) {
            Fail(
                SessionLoadStatus::CorruptData,
                SessionErrorCode::CpuScopePayloadInvalid,
                "CpuScope record fields do not match the registered template",
                DecodeStatus::MalformedPayload
            );
            return false;
        }
        if (*end_ns < *begin_ns) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::CpuScopeTimeInvalid,
                "CpuScope end precedes begin"
            );
            return false;
        }
        if (*depth > options.limits.max_scope_depth) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "CpuScope depth limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::ScopeDepth
            );
            return false;
        }
        if (!CheckCountLimit(
                session.impl_->cpu_scopes.size(),
                options.limits.max_cpu_scopes,
                SessionLimitKind::CpuScopes,
                "CpuScope record limit exceeded"
            ) ||
            !ChargeModel(sizeof(CpuScopeRecord))) {
            return false;
        }

        SessionStringId name_id = kInvalidSessionString;
        if (!InternString(*name, name_id)) {
            return false;
        }
        session.impl_->cpu_scopes.push_back({
            .sequence  = _record.sequence,
            .thread_id = *thread_id,
            .name      = name_id,
            .begin_ns  = *begin_ns,
            .end_ns    = *end_ns,
            .depth     = *depth,
        });

        ProfileSessionSummary& summary = session.impl_->summary;
        ++summary.cpu_scope_count;
        if (!summary.has_cpu_range) {
            summary.has_cpu_range = true;
            summary.cpu_begin_ns  = *begin_ns;
            summary.cpu_end_ns    = *end_ns;
        } else {
            summary.cpu_begin_ns = std::min(summary.cpu_begin_ns, *begin_ns);
            summary.cpu_end_ns   = std::max(summary.cpu_end_ns, *end_ns);
        }
        return true;
    }

    [[nodiscard]] bool MaterializeGpuFrame(const DecodedRecord& _record) {
        const auto* frame_id       = RecordField<std::uint64_t>(_record, 0);
        const auto* capture_status = RecordField<std::uint32_t>(_record, 1);
        const auto* valid          = RecordField<bool>(_record, 2);
        const auto* admitted       = RecordField<std::uint64_t>(_record, 3);
        const auto* dropped        = RecordField<std::uint64_t>(_record, 4);
        const auto* errors         = RecordField<std::uint64_t>(_record, 5);
        const auto* reason         = RecordField<ProfileString>(_record, 6);
        if (frame_id == nullptr || capture_status == nullptr || valid == nullptr || admitted == nullptr ||
            dropped == nullptr || errors == nullptr || reason == nullptr) {
            Fail(
                SessionLoadStatus::CorruptData,
                SessionErrorCode::GpuFramePayloadInvalid,
                "GpuFrame record fields do not match the registered template",
                DecodeStatus::MalformedPayload
            );
            return false;
        }
        if (*capture_status > static_cast<std::uint32_t>(ProfileGpuFrameStatus::Invalid)) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuFrameStatusInvalid,
                "GpuFrame capture status is invalid"
            );
            return false;
        }
        const ProfileGpuFrameStatus status = static_cast<ProfileGpuFrameStatus>(*capture_status);
        if ((status == ProfileGpuFrameStatus::Complete) != *valid) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuFrameStatusInvalid,
                "GpuFrame validity disagrees with capture status"
            );
            return false;
        }
        const bool frame_status_consistent =
            *errors <= *admitted &&
            (status != ProfileGpuFrameStatus::Complete || (*dropped == 0 && *errors == 0)) &&
            (status != ProfileGpuFrameStatus::Incomplete || *errors == 0);
        if (!frame_status_consistent) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuFrameStatusInvalid,
                "GpuFrame counters disagree with capture status"
            );
            return false;
        }
        if (!CheckCountLimit(
                session.impl_->gpu_frames.size(),
                options.limits.max_gpu_frames,
                SessionLimitKind::GpuFrames,
                "GpuFrame record limit exceeded"
            ) ||
            !ChargeModel(sizeof(GpuFrameRecord))) {
            return false;
        }

        SessionStringId reason_id = kInvalidSessionString;
        if (!InternString(*reason, reason_id)) {
            return false;
        }
        session.impl_->gpu_frames.push_back({
            .sequence             = _record.sequence,
            .frame_id             = *frame_id,
            .status               = status,
            .valid                = *valid,
            .admitted_scope_count = *admitted,
            .dropped_scope_count  = *dropped,
            .error_scope_count    = *errors,
            .reason               = reason_id,
        });

        ProfileSessionSummary& summary = session.impl_->summary;
        ++summary.gpu_frame_count;
        switch (status) {
            case ProfileGpuFrameStatus::Complete:
                ++summary.complete_gpu_frame_count;
                break;
            case ProfileGpuFrameStatus::Incomplete:
                ++summary.incomplete_gpu_frame_count;
                break;
            case ProfileGpuFrameStatus::Invalid:
                ++summary.invalid_gpu_frame_count;
                break;
        }
        return true;
    }

    [[nodiscard]] bool MaterializeGpuScope(const DecodedRecord& _record) {
        const auto* frame_id              = RecordField<std::uint64_t>(_record, 0);
        const auto* scope_id              = RecordField<std::uint64_t>(_record, 1);
        const auto* parent_scope_id       = RecordField<std::uint64_t>(_record, 2);
        const auto* source_order          = RecordField<std::uint64_t>(_record, 3);
        const auto* local_order           = RecordField<std::uint64_t>(_record, 4);
        const auto* logical_queue         = RecordField<std::uint32_t>(_record, 5);
        const auto* native_queue_id       = RecordField<std::uint32_t>(_record, 6);
        const auto* family_id             = RecordField<std::uint32_t>(_record, 7);
        const auto* name                  = RecordField<ProfileString>(_record, 8);
        const auto* encoded_status        = RecordField<std::uint32_t>(_record, 9);
        const auto* begin_tick            = RecordField<std::uint64_t>(_record, 10);
        const auto* end_tick              = RecordField<std::uint64_t>(_record, 11);
        const auto* valid_bits            = RecordField<std::uint32_t>(_record, 12);
        const auto* tick_period_ns        = RecordField<double>(_record, 13);
        const auto* total_duration_ns     = RecordField<double>(_record, 14);
        const auto* exclusive_duration_ns = RecordField<double>(_record, 15);
        const auto* depth                 = RecordField<std::uint32_t>(_record, 16);
        const auto* error_reason          = RecordField<ProfileString>(_record, 17);
        if (frame_id == nullptr || scope_id == nullptr || parent_scope_id == nullptr ||
            source_order == nullptr || local_order == nullptr || logical_queue == nullptr ||
            native_queue_id == nullptr || family_id == nullptr || name == nullptr ||
            encoded_status == nullptr || begin_tick == nullptr || end_tick == nullptr ||
            valid_bits == nullptr || tick_period_ns == nullptr || total_duration_ns == nullptr ||
            exclusive_duration_ns == nullptr || depth == nullptr || error_reason == nullptr) {
            Fail(
                SessionLoadStatus::CorruptData,
                SessionErrorCode::GpuScopePayloadInvalid,
                "GpuScope record fields do not match the registered template",
                DecodeStatus::MalformedPayload
            );
            return false;
        }
        if (*scope_id == 0 || *scope_id == *parent_scope_id) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuScopeIdentityInvalid,
                "GpuScope identity or parent identity is invalid"
            );
            return false;
        }
        if (*logical_queue > static_cast<std::uint32_t>(ProfileLogicalQueue::Copy)) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuScopeQueueInvalid,
                "GpuScope logical queue is invalid"
            );
            return false;
        }
        if (*encoded_status > static_cast<std::uint32_t>(ProfileGpuScopeStatus::Error)) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::GpuScopeStatusInvalid,
                "GpuScope terminal status is invalid"
            );
            return false;
        }
        if (*depth > options.limits.max_scope_depth) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "GpuScope depth limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::ScopeDepth
            );
            return false;
        }

        const ProfileGpuScopeStatus status = static_cast<ProfileGpuScopeStatus>(*encoded_status);
        if (status == ProfileGpuScopeStatus::Ready) {
            bool timing_valid = *valid_bits != 0 && *valid_bits <= 64 && std::isfinite(*tick_period_ns) &&
                                *tick_period_ns > 0.0 && IsFiniteNonnegative(*total_duration_ns) &&
                                IsFiniteNonnegative(*exclusive_duration_ns) &&
                                DurationContains(*total_duration_ns, *exclusive_duration_ns);
            if (timing_valid) {
                const double expected_duration_ns =
                    static_cast<double>(TimestampDelta(*begin_tick, *end_tick, *valid_bits)) *
                    *tick_period_ns;
                timing_valid = NearlyEqualDuration(*total_duration_ns, expected_duration_ns);
            }
            if (!timing_valid) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeTimingInvalid,
                    "ready GpuScope timing metadata is invalid"
                );
                return false;
            }
        }
        if (!CheckCountLimit(
                session.impl_->gpu_scopes.size(),
                options.limits.max_gpu_scopes,
                SessionLimitKind::GpuScopes,
                "GpuScope record limit exceeded"
            ) ||
            !ChargeModel(sizeof(GpuScopeRecord))) {
            return false;
        }

        SessionStringId name_id  = kInvalidSessionString;
        SessionStringId error_id = kInvalidSessionString;
        if (!InternString(*name, name_id) || !InternString(*error_reason, error_id)) {
            return false;
        }
        session.impl_->gpu_scopes.push_back({
            .sequence              = _record.sequence,
            .frame_id              = *frame_id,
            .scope_id              = *scope_id,
            .parent_scope_id       = *parent_scope_id,
            .source_order          = *source_order,
            .local_order           = *local_order,
            .logical_queue         = static_cast<ProfileLogicalQueue>(*logical_queue),
            .native_queue_id       = *native_queue_id,
            .family_id             = *family_id,
            .name                  = name_id,
            .status                = status,
            .begin_tick            = *begin_tick,
            .end_tick              = *end_tick,
            .valid_bits            = *valid_bits,
            .tick_period_ns        = *tick_period_ns,
            .total_duration_ns     = *total_duration_ns,
            .exclusive_duration_ns = *exclusive_duration_ns,
            .depth                 = *depth,
            .error_reason          = error_id,
        });

        ProfileSessionSummary& summary = session.impl_->summary;
        ++summary.gpu_scope_count;
        if (status == ProfileGpuScopeStatus::Ready) {
            ++summary.ready_gpu_scope_count;
        } else {
            ++summary.error_gpu_scope_count;
        }
        return true;
    }

    [[nodiscard]] bool ProcessSchema(const PacketView& _packet) {
        SchemaDescriptor   descriptor{};
        const DecodeStatus decode = DecodeSchemaPayload(_packet, options.limits.codec, descriptor);
        if (decode != DecodeStatus::Ok) {
            FailCodec(
                decode, SessionErrorCode::SchemaDecodeInvalid, "profile schema packet failed to decode"
            );
            return false;
        }

        const std::uint64_t hash     = ComputeSchemaHash(descriptor);
        const auto          existing = schemas.find(hash);
        ++session.impl_->summary.schema_packet_count;
        if (existing != schemas.end()) {
            if (existing->second.descriptor != descriptor) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::SchemaConflict,
                    "conflicting profile schemas share one hash",
                    DecodeStatus::SchemaHashMismatch
                );
                return false;
            }
            return true;
        }

        if (!CheckCountLimit(
                schemas.size(),
                options.limits.max_schemas,
                SessionLimitKind::Schemas,
                "profile schema count limit exceeded"
            )) {
            return false;
        }
        std::uint64_t next_schema_bytes = 0;
        if (AddOverflow(
                schema_bytes, static_cast<std::uint64_t>(_packet.payload.size()), next_schema_bytes
            ) ||
            next_schema_bytes > options.limits.max_schema_bytes) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile schema byte limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::SchemaBytes
            );
            return false;
        }
        if (!ChargeModel(sizeof(ProfileSchemaInfo) + static_cast<std::uint64_t>(_packet.payload.size()))) {
            return false;
        }

        SessionStringId name       = kInvalidSessionString;
        SessionStringId event_type = kInvalidSessionString;
        if (!InternString(descriptor.name, name) || !InternString(descriptor.event_type, event_type)) {
            return false;
        }

        const std::uint64_t first_field = session.impl_->schema_fields.size();
        for (const SchemaField& field : descriptor.fields) {
            if (!ChargeModel(sizeof(ProfileSchemaFieldInfo))) {
                return false;
            }
            SessionStringId field_name = kInvalidSessionString;
            if (!InternString(field.name, field_name)) {
                return false;
            }
            session.impl_->schema_fields.push_back({
                .name = field_name,
                .type = field.type,
            });
        }

        const KnownProfileSchema known      = IdentifyKnownSchema(descriptor);
        const std::size_t        info_index = session.impl_->schemas.size();
        session.impl_->schemas.push_back({
            .hash           = hash,
            .name           = name,
            .event_type     = event_type,
            .schema_version = descriptor.schema_version,
            .kind           = descriptor.kind,
            .channel        = descriptor.channel,
            .known_schema   = known,
            .first_field    = first_field,
            .field_count    = static_cast<std::uint32_t>(descriptor.fields.size()),
        });
        schemas.emplace(
            hash,
            SchemaEntry{
                .descriptor   = std::move(descriptor),
                .known_schema = known,
                .info_index   = info_index,
            }
        );
        schema_bytes                               = next_schema_bytes;
        session.impl_->summary.unique_schema_count = schemas.size();
        return true;
    }

    [[nodiscard]] bool ProcessRecord(const PacketView& _packet) {
        if (_packet.payload.size() < sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t)) {
            Fail(
                SessionLoadStatus::CorruptData,
                SessionErrorCode::RecordPayloadInvalid,
                "profile record prefix is truncated",
                DecodeStatus::MalformedPayload
            );
            return false;
        }
        if (!CheckCountLimit(
                session.impl_->summary.record_count,
                options.limits.max_records,
                SessionLimitKind::Records,
                "profile record limit exceeded"
            )) {
            return false;
        }

        const std::uint64_t schema_hash = ReadU64LittleEndian(_packet.payload, 0);
        const auto          schema      = schemas.find(schema_hash);
        if (schema == schemas.end()) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSchemaUnregistered,
                "profile record references an unregistered schema",
                DecodeStatus::UnknownSchema
            );
            return false;
        }

        DecodedRecord      record{};
        const DecodeStatus decode =
            DecodeRecordPayload(_packet, schema->second.descriptor, options.limits.codec, record);
        if (decode != DecodeStatus::Ok) {
            FailCodec(
                decode, SessionErrorCode::RecordPayloadInvalid, "profile record packet failed to decode"
            );
            return false;
        }

        switch (schema->second.known_schema) {
            case KnownProfileSchema::CpuScopeV1:
                if (!MaterializeCpuScope(record)) {
                    return false;
                }
                break;
            case KnownProfileSchema::GpuFrameV1:
                if (!MaterializeGpuFrame(record)) {
                    return false;
                }
                break;
            case KnownProfileSchema::GpuScopeV2:
                if (!MaterializeGpuScope(record)) {
                    return false;
                }
                break;
            case KnownProfileSchema::Unknown:
                ++session.impl_->summary.unknown_record_count;
                break;
        }

        record_sequences.push_back(record.sequence);
        ++session.impl_->summary.record_count;
        ++session.impl_->schemas[schema->second.info_index].record_count;
        return true;
    }

    [[nodiscard]] bool ProcessLoss(const PacketView& _packet) {
        if (!CheckCountLimit(
                session.impl_->losses.size(),
                options.limits.max_loss_notices,
                SessionLimitKind::LossNotices,
                "profile loss notice limit exceeded"
            ) ||
            !ChargeModel(sizeof(ProfileLossRecord))) {
            return false;
        }

        LossNotice         loss{};
        const DecodeStatus decode = DecodeLossPayload(_packet, loss);
        if (decode != DecodeStatus::Ok) {
            FailCodec(decode, SessionErrorCode::LossPayloadInvalid, "profile loss packet failed to decode");
            return false;
        }

        ProfileSessionSummary& summary      = session.impl_->summary;
        std::uint64_t          lost_records = 0;
        std::uint64_t          lost_bytes   = 0;
        if (AddOverflow(summary.lost_record_count, loss.record_count, lost_records) ||
            AddOverflow(summary.lost_value_bytes, loss.value_bytes, lost_bytes)) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::LossTotalsOverflow,
                "profile loss totals overflow"
            );
            return false;
        }
        session.impl_->losses.push_back({
            .first_sequence = loss.first_sequence,
            .last_sequence  = loss.last_sequence,
            .record_count   = loss.record_count,
            .value_bytes    = loss.value_bytes,
            .reason_mask    = loss.reason_mask,
        });
        summary.lost_record_count = lost_records;
        summary.lost_value_bytes  = lost_bytes;
        summary.loss_reason_mask |= loss.reason_mask;
        ++summary.loss_notice_count;
        return true;
    }

    [[nodiscard]] bool ValidateLossSequenceAllocation() {
        if (session.impl_->losses.empty()) {
            return true;
        }

        struct SequenceDemand {
            std::uint64_t release_sequence{0};
            std::uint64_t deadline_sequence{0};
            std::uint64_t count{0};
        };

        const std::uint64_t loss_notice_count = static_cast<std::uint64_t>(session.impl_->losses.size());
        if (!ChargeTopologyWork(loss_notice_count)) {
            return false;
        }

        std::uint64_t mandatory_sequence_count = 0;
        std::uint64_t residual_demand_count    = 0;
        for (const ProfileLossRecord& loss : session.impl_->losses) {
            if (loss.first_sequence == 0 ||
                (loss.record_count == 1 && loss.first_sequence != loss.last_sequence)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::LossPayloadInvalid,
                    "profile loss notice sequence extrema are not emitted-record sequences"
                );
                return false;
            }

            std::uint64_t mandatory_for_loss = 1;
            if (loss.record_count != 1) {
                ++mandatory_for_loss;
            }
            if (loss.record_count > 2) {
                ++residual_demand_count;
            }
            if (AddOverflow(mandatory_sequence_count, mandatory_for_loss, mandatory_sequence_count)) {
                Fail(
                    SessionLoadStatus::LimitExceeded,
                    SessionErrorCode::LimitExceeded,
                    "profile loss allocation preprocessing overflows",
                    DecodeStatus::LimitExceeded,
                    SessionLimitKind::TopologyWorkItems
                );
                return false;
            }
        }

        std::uint64_t construction_work = 0;
        if (AddOverflow(mandatory_sequence_count, residual_demand_count, construction_work) ||
            mandatory_sequence_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            residual_demand_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "profile loss allocation model exceeds the addressable range",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        if (!ChargeTopologyWork(construction_work)) {
            return false;
        }

        std::vector<std::uint64_t> mandatory_sequences;
        mandatory_sequences.reserve(static_cast<std::size_t>(mandatory_sequence_count));
        std::vector<SequenceDemand> residual_demands;
        residual_demands.reserve(static_cast<std::size_t>(residual_demand_count));
        for (const ProfileLossRecord& loss : session.impl_->losses) {
            mandatory_sequences.push_back(loss.first_sequence);
            if (loss.record_count != 1) {
                mandatory_sequences.push_back(loss.last_sequence);
            }
            if (loss.record_count > 2) {
                residual_demands.push_back({
                    .release_sequence  = loss.first_sequence + 1,
                    .deadline_sequence = loss.last_sequence - 1,
                    .count             = loss.record_count - 2,
                });
            }
        }

        const auto charge_sort_work = [&](std::uint64_t _count) {
            std::uint64_t width = 1;
            while (width < _count) {
                if (!ChargeTopologyWork(_count)) {
                    return false;
                }
                if (width > std::numeric_limits<std::uint64_t>::max() / 2) {
                    break;
                }
                width *= 2;
            }
            return true;
        };
        const auto charge_binary_search = [&](std::size_t _count) {
            std::uint64_t work      = 0;
            std::size_t   remaining = _count;
            while (remaining != 0) {
                ++work;
                remaining /= 2;
            }
            return ChargeTopologyWork(work);
        };

        if (!charge_sort_work(mandatory_sequence_count)) {
            return false;
        }
        std::sort(mandatory_sequences.begin(), mandatory_sequences.end());
        if (!ChargeTopologyWork(mandatory_sequence_count)) {
            return false;
        }
        if (std::adjacent_find(mandatory_sequences.begin(), mandatory_sequences.end()) !=
            mandatory_sequences.end()) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSequenceInvalid,
                "profile records and loss notices claim the same allocated sequence"
            );
            return false;
        }
        if (!ChargeTopologyWork(mandatory_sequence_count)) {
            return false;
        }
        for (const std::uint64_t sequence : mandatory_sequences) {
            if (!charge_binary_search(record_sequences.size())) {
                return false;
            }
            const auto record = std::lower_bound(record_sequences.begin(), record_sequences.end(), sequence);
            if (record != record_sequences.end() && *record == sequence) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::RecordSequenceInvalid,
                    "profile records and loss notices claim the same allocated sequence"
                );
                return false;
            }
        }

        if (residual_demands.empty()) {
            return true;
        }
        if (!charge_sort_work(residual_demand_count)) {
            return false;
        }
        std::sort(
            residual_demands.begin(),
            residual_demands.end(),
            [](const SequenceDemand& _left, const SequenceDemand& _right) {
                if (_left.release_sequence != _right.release_sequence) {
                    return _left.release_sequence < _right.release_sequence;
                }
                return _left.deadline_sequence < _right.deadline_sequence;
            }
        );

        struct PendingDemand {
            std::uint64_t deadline_sequence{0};
            std::uint64_t remaining_count{0};
        };
        const auto later_deadline = [](const PendingDemand& _left, const PendingDemand& _right) {
            return _left.deadline_sequence > _right.deadline_sequence;
        };
        std::vector<PendingDemand> pending_storage;
        pending_storage.reserve(residual_demands.size());
        std::priority_queue<PendingDemand, std::vector<PendingDemand>, decltype(later_deadline)>
                   pending_demands(later_deadline, std::move(pending_storage));
        const auto charge_heap_work = [&](std::size_t _size) {
            std::uint64_t work = 1;
            while (_size > 1) {
                ++work;
                _size /= 2;
            }
            return ChargeTopologyWork(work);
        };

        const auto fail_allocation = [&]() {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSequenceInvalid,
                "profile loss notices cannot claim distinct allocated record sequences"
            );
            return false;
        };

        std::size_t   demand_index             = 0;
        std::size_t   record_index             = 0;
        std::size_t   mandatory_index          = 0;
        std::uint64_t position                 = residual_demands.front().release_sequence;
        bool          sequence_space_exhausted = false;
        while (demand_index < residual_demands.size() || !pending_demands.empty()) {
            if (!ChargeTopologyWork(1)) {
                return false;
            }
            if (sequence_space_exhausted) {
                return fail_allocation();
            }
            while (demand_index < residual_demands.size() &&
                   residual_demands[demand_index].release_sequence <= position) {
                if (!charge_heap_work(pending_demands.size() + 1)) {
                    return false;
                }
                pending_demands.push({
                    .deadline_sequence = residual_demands[demand_index].deadline_sequence,
                    .remaining_count   = residual_demands[demand_index].count,
                });
                ++demand_index;
            }
            if (pending_demands.empty()) {
                position = residual_demands[demand_index].release_sequence;
                continue;
            }
            if (pending_demands.top().deadline_sequence < position) {
                return fail_allocation();
            }

            if (record_index < record_sequences.size() && record_sequences[record_index] < position) {
                if (!charge_binary_search(record_sequences.size() - record_index)) {
                    return false;
                }
                record_index = static_cast<std::size_t>(
                    std::lower_bound(
                        record_sequences.begin() + record_index, record_sequences.end(), position
                    ) -
                    record_sequences.begin()
                );
            }
            while (mandatory_index < mandatory_sequences.size() &&
                   mandatory_sequences[mandatory_index] < position) {
                if (!ChargeTopologyWork(1)) {
                    return false;
                }
                ++mandatory_index;
            }

            const bool          has_record    = record_index < record_sequences.size();
            const bool          has_mandatory = mandatory_index < mandatory_sequences.size();
            const std::uint64_t next_record =
                has_record ? record_sequences[record_index] : std::numeric_limits<std::uint64_t>::max();
            const std::uint64_t next_mandatory = has_mandatory ? mandatory_sequences[mandatory_index] :
                                                                 std::numeric_limits<std::uint64_t>::max();
            const bool          has_occupied   = has_record || has_mandatory;
            const std::uint64_t next_occupied  = std::min(next_record, next_mandatory);
            if (has_occupied && next_occupied == position) {
                if (position == std::numeric_limits<std::uint64_t>::max()) {
                    return fail_allocation();
                }
                if (has_record && next_record == position) {
                    ++record_index;
                } else {
                    ++mandatory_index;
                }
                ++position;
                continue;
            }

            if (!charge_heap_work(pending_demands.size())) {
                return false;
            }
            PendingDemand demand = pending_demands.top();
            pending_demands.pop();
            std::uint64_t batch_end = demand.deadline_sequence;
            if (has_occupied && next_occupied > position) {
                batch_end = std::min(batch_end, next_occupied - 1);
            }
            if (demand_index < residual_demands.size() &&
                residual_demands[demand_index].release_sequence > position) {
                batch_end = std::min(batch_end, residual_demands[demand_index].release_sequence - 1);
            }
            if (batch_end < position) {
                return fail_allocation();
            }

            const std::uint64_t available = batch_end - position + 1;
            const std::uint64_t assigned  = std::min(available, demand.remaining_count);
            demand.remaining_count -= assigned;
            if (assigned < available) {
                position += assigned;
            } else if (batch_end == std::numeric_limits<std::uint64_t>::max()) {
                sequence_space_exhausted = true;
            } else {
                position = batch_end + 1;
            }
            if (demand.remaining_count != 0) {
                if (!charge_heap_work(pending_demands.size() + 1)) {
                    return false;
                }
                pending_demands.push(demand);
            }
        }
        return true;
    }

    [[nodiscard]] bool ProcessPacket(const PacketView& _packet) {
        if (!CheckCountLimit(
                session.impl_->summary.packet_count,
                options.limits.max_packets,
                SessionLimitKind::Packets,
                "profile packet limit exceeded"
            )) {
            return false;
        }
        if (_packet.header.packet_index != expected_packet_index) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::PacketIndexMismatch,
                "profile packet indices are not contiguous"
            );
            return false;
        }
        if (!saw_session_begin && _packet.header.type != PacketType::SessionBegin) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::SessionBeginMissing,
                "profile session does not begin with SessionBegin"
            );
            return false;
        }
        if (saw_session_end) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::TrailingDataAfterSessionEnd,
                "profile packet appears after SessionEnd"
            );
            return false;
        }

        switch (_packet.header.type) {
            case PacketType::SessionBegin: {
                if (saw_session_begin || _packet.header.packet_index != 0) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::SessionBeginDuplicate,
                        "profile session contains a duplicate SessionBegin"
                    );
                    return false;
                }
                SessionBeginInfo   begin{};
                const DecodeStatus decode = DecodeSessionBeginPayload(_packet, begin);
                if (decode != DecodeStatus::Ok) {
                    FailCodec(
                        decode, SessionErrorCode::SessionBeginMissing, "SessionBegin payload failed to decode"
                    );
                    return false;
                }
                saw_session_begin                      = true;
                session.impl_->summary.generation      = begin.generation;
                session.impl_->summary.started_unix_ns = begin.started_unix_ns;
                break;
            }
            case PacketType::Schema:
                if (!ProcessSchema(_packet)) {
                    return false;
                }
                break;
            case PacketType::Record:
                if (!ProcessRecord(_packet)) {
                    return false;
                }
                break;
            case PacketType::Loss:
                if (!ProcessLoss(_packet)) {
                    return false;
                }
                break;
            case PacketType::SessionEnd: {
                SessionEndInfo     end{};
                const DecodeStatus decode = DecodeSessionEndPayload(_packet, end);
                if (decode != DecodeStatus::Ok) {
                    FailCodec(
                        decode,
                        SessionErrorCode::SessionEndTotalsMismatch,
                        "SessionEnd payload failed to decode"
                    );
                    return false;
                }
                const ProfileSessionSummary& summary = session.impl_->summary;
                if (end.generation != summary.generation || end.records_written != summary.record_count ||
                    end.records_dropped < summary.lost_record_count) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::SessionEndTotalsMismatch,
                        "SessionEnd totals do not match the decoded session"
                    );
                    return false;
                }
                saw_session_end                                    = true;
                session.impl_->summary.has_session_end             = true;
                session.impl_->summary.session_end_records_written = end.records_written;
                session.impl_->summary.session_end_records_dropped = end.records_dropped;
                session.impl_->summary.unnotified_drop_count =
                    end.records_dropped - summary.lost_record_count;
                break;
            }
        }

        ++session.impl_->summary.packet_count;
        ++expected_packet_index;
        result.packet_count = session.impl_->summary.packet_count;
        return true;
    }

    [[nodiscard]] bool BuildCpuTracks(
        bool                               _allow_missing,
        std::uint64_t&                     _required_cpu_drops,
        std::vector<RecordSequenceDemand>& _sequence_demands
    ) {
        _required_cpu_drops = 0;
        _sequence_demands.clear();
        auto& scopes = session.impl_->cpu_scopes;
        std::sort(
            scopes.begin(),
            scopes.end(),
            [](const CpuScopeRecord& _left, const CpuScopeRecord& _right) {
                if (_left.thread_id != _right.thread_id) {
                    return _left.thread_id < _right.thread_id;
                }
                if (_left.begin_ns != _right.begin_ns) {
                    return _left.begin_ns < _right.begin_ns;
                }
                if (_left.end_ns != _right.end_ns) {
                    return _left.end_ns > _right.end_ns;
                }
                if (_left.depth != _right.depth) {
                    return _left.depth < _right.depth;
                }
                return _left.sequence < _right.sequence;
            }
        );

        struct CpuGapEvent {
            std::uint64_t ancestor_index{0};
            std::uint64_t scope_index{0};
            std::uint32_t depth_gap{0};
        };
        std::vector<CpuGapEvent> gap_events;
        gap_events.reserve(scopes.size());
        std::size_t track_begin = 0;
        while (track_begin < scopes.size()) {
            std::size_t track_end = track_begin + 1;
            while (track_end < scopes.size() && scopes[track_end].thread_id == scopes[track_begin].thread_id
            ) {
                ++track_end;
            }
            if (!CheckCountLimit(
                    session.impl_->cpu_tracks.size(),
                    options.limits.max_cpu_tracks,
                    SessionLimitKind::CpuTracks,
                    "CPU track limit exceeded"
                ) ||
                !ChargeModel(sizeof(CpuTrack))) {
                return false;
            }
            const std::uint32_t track_index = static_cast<std::uint32_t>(session.impl_->cpu_tracks.size());
            session.impl_->cpu_tracks.push_back({
                .thread_id   = scopes[track_begin].thread_id,
                .first_scope = track_begin,
                .scope_count = track_end - track_begin,
            });

            std::vector<std::uint64_t> active;
            active.reserve(std::min<std::size_t>(
                track_end - track_begin, static_cast<std::size_t>(options.limits.max_scope_depth) + 1
            ));
            std::map<std::uint32_t, std::vector<std::uint64_t>> ending_parents_by_depth;
            std::uint64_t                                       boundary_time     = 0;
            bool                                                has_boundary_time = false;
            for (std::size_t index = track_begin; index < track_end; ++index) {
                CpuScopeRecord& scope = scopes[index];
                scope.track_index     = track_index;

                if (!has_boundary_time || boundary_time != scope.begin_ns) {
                    ending_parents_by_depth.clear();
                    boundary_time     = scope.begin_ns;
                    has_boundary_time = true;
                }
                while (!active.empty()) {
                    const std::uint64_t   candidate_index = active.back();
                    const CpuScopeRecord& candidate       = scopes[candidate_index];
                    const bool            zero_duration_child_at_end =
                        candidate.end_ns == scope.begin_ns && scope.end_ns == scope.begin_ns &&
                        scope.depth > candidate.depth && candidate.sequence > scope.sequence;
                    if (candidate.end_ns > scope.begin_ns || zero_duration_child_at_end) {
                        break;
                    }
                    if (candidate.sequence >= scope.sequence) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::CpuScopeTopologyInvalid,
                            "non-overlapping CpuScopes move backward in record sequence"
                        );
                        return false;
                    }
                    if (candidate.end_ns == scope.begin_ns) {
                        ending_parents_by_depth[candidate.depth].push_back(candidate_index);
                    }
                    active.pop_back();
                }

                if (!active.empty()) {
                    const CpuScopeRecord& candidate     = scopes[active.back()];
                    const bool            crossing      = scope.end_ns > candidate.end_ns;
                    const bool            invalid_depth = scope.depth <= candidate.depth;
                    if (crossing || invalid_depth) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::CpuScopeTopologyInvalid,
                            "CpuScope intervals overlap without a valid nesting relation"
                        );
                        return false;
                    }
                }

                std::uint64_t ancestor_index = active.empty() ? kInvalidSessionIndex : active.back();
                if (scope.begin_ns == scope.end_ns && scope.depth != 0) {
                    std::uint64_t ending_ancestor_index = kInvalidSessionIndex;
                    auto          ending_depth          = ending_parents_by_depth.lower_bound(scope.depth);
                    while (ending_depth != ending_parents_by_depth.begin()) {
                        --ending_depth;
                        if (!ChargeTopologyWork(static_cast<std::uint64_t>(ending_depth->second.size()))) {
                            return false;
                        }
                        for (const std::uint64_t candidate_index : ending_depth->second) {
                            const CpuScopeRecord& candidate = scopes[candidate_index];
                            if (candidate.sequence > scope.sequence &&
                                (ending_ancestor_index == kInvalidSessionIndex ||
                                 candidate.sequence < scopes[ending_ancestor_index].sequence)) {
                                ending_ancestor_index = candidate_index;
                            }
                        }
                        if (ending_ancestor_index != kInvalidSessionIndex) {
                            break;
                        }
                    }
                    if (ending_ancestor_index != kInvalidSessionIndex &&
                        (ancestor_index == kInvalidSessionIndex ||
                         scopes[ending_ancestor_index].depth > scopes[ancestor_index].depth ||
                         (scopes[ending_ancestor_index].depth == scopes[ancestor_index].depth &&
                          scopes[ending_ancestor_index].sequence < scopes[ancestor_index].sequence))) {
                        ancestor_index = ending_ancestor_index;
                    }
                }

                std::uint64_t parent_index = kInvalidSessionIndex;
                if (ancestor_index != kInvalidSessionIndex) {
                    const CpuScopeRecord& ancestor = scopes[ancestor_index];
                    if (ancestor.sequence <= scope.sequence) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::CpuScopeTopologyInvalid,
                            "CpuScope containing ancestor was emitted before its descendant"
                        );
                        return false;
                    }
                    const std::uint32_t depth_gap      = scope.depth - ancestor.depth - 1;
                    const std::uint64_t sequence_slots = ancestor.sequence - scope.sequence - 1;
                    if (sequence_slots < depth_gap) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::CpuScopeTopologyInvalid,
                            "CpuScope depth gap has too few intervening record sequences"
                        );
                        return false;
                    }
                    gap_events.push_back({
                        .ancestor_index = ancestor_index,
                        .scope_index    = index,
                        .depth_gap      = depth_gap,
                    });
                    if (depth_gap == 0) {
                        parent_index = ancestor_index;
                    }
                }

                if (scope.depth != 0 && parent_index == kInvalidSessionIndex) {
                    ++session.impl_->summary.orphan_cpu_scope_count;
                } else if (parent_index != kInvalidSessionIndex) {
                    scope.parent_index = parent_index;
                }
                active.push_back(index);
            }
            track_begin = track_end;
        }

        _sequence_demands.reserve(gap_events.size());
        const auto add_sequence_demand =
            [&](std::uint64_t _lower_sequence, std::uint64_t _upper_sequence, std::uint64_t _count) {
                if (_count == 0) {
                    return true;
                }
                if (_lower_sequence >= _upper_sequence || _upper_sequence - _lower_sequence - 1 < _count) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::CpuScopeTopologyInvalid,
                        "CpuScope hierarchy has too few record-sequence slots for missing parents"
                    );
                    return false;
                }
                std::uint64_t required_cpu_drops = 0;
                if (AddOverflow(_required_cpu_drops, _count, required_cpu_drops)) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::CpuScopeTopologyInvalid,
                        "minimum CPU record loss count overflows"
                    );
                    return false;
                }
                _required_cpu_drops = required_cpu_drops;
                _sequence_demands.push_back({
                    .release_sequence  = _lower_sequence + 1,
                    .deadline_sequence = _upper_sequence - 1,
                    .count             = _count,
                });
                return true;
            };

        std::sort(
            gap_events.begin(),
            gap_events.end(),
            [](const CpuGapEvent& _left, const CpuGapEvent& _right) {
                if (_left.ancestor_index != _right.ancestor_index) {
                    return _left.ancestor_index < _right.ancestor_index;
                }
                return _left.scope_index < _right.scope_index;
            }
        );
        std::size_t event_begin = 0;
        while (event_begin < gap_events.size()) {
            std::size_t event_end = event_begin + 1;
            while (event_end < gap_events.size() &&
                   gap_events[event_end].ancestor_index == gap_events[event_begin].ancestor_index) {
                ++event_end;
            }

            std::uint32_t previous_gap      = gap_events[event_begin].depth_gap;
            std::uint64_t previous_sequence = scopes[gap_events[event_begin].scope_index].sequence;
            for (std::size_t index = event_begin + 1; index < event_end; ++index) {
                const CpuGapEvent&  event          = gap_events[index];
                const std::uint64_t event_sequence = scopes[event.scope_index].sequence;
                if (event_sequence <= previous_sequence) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::CpuScopeTopologyInvalid,
                        "compressed CpuScope siblings move backward in record sequence"
                    );
                    return false;
                }
                if (event.depth_gap < previous_gap &&
                    !add_sequence_demand(
                        previous_sequence,
                        event_sequence,
                        static_cast<std::uint64_t>(previous_gap - event.depth_gap)
                    )) {
                    return false;
                }
                previous_gap      = event.depth_gap;
                previous_sequence = event_sequence;
            }

            const std::uint64_t ancestor_sequence = scopes[gap_events[event_begin].ancestor_index].sequence;
            if (!add_sequence_demand(previous_sequence, ancestor_sequence, previous_gap)) {
                return false;
            }
            event_begin = event_end;
        }

        if (_required_cpu_drops != 0 && !_allow_missing) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::CpuScopeParentMissing,
                "CpuScope hierarchy requires missing emitted parents"
            );
            return false;
        }

        std::sort(
            _sequence_demands.begin(),
            _sequence_demands.end(),
            [](const RecordSequenceDemand& _left, const RecordSequenceDemand& _right) {
                if (_left.release_sequence != _right.release_sequence) {
                    return _left.release_sequence < _right.release_sequence;
                }
                return _left.deadline_sequence < _right.deadline_sequence;
            }
        );
        struct PendingCpuSequenceDemand {
            std::uint64_t deadline_sequence{0};
            std::uint64_t remaining_count{0};
        };
        const auto later_deadline = [](const PendingCpuSequenceDemand& _left,
                                       const PendingCpuSequenceDemand& _right) {
            return _left.deadline_sequence > _right.deadline_sequence;
        };
        std::priority_queue<
            PendingCpuSequenceDemand,
            std::vector<PendingCpuSequenceDemand>,
            decltype(later_deadline)>
                      pending_demands(later_deadline);
        std::size_t   demand_index   = 0;
        std::size_t   observed_index = 0;
        std::uint64_t position = _sequence_demands.empty() ? 0 : _sequence_demands.front().release_sequence;
        while (demand_index < _sequence_demands.size() || !pending_demands.empty()) {
            while (demand_index < _sequence_demands.size() &&
                   _sequence_demands[demand_index].release_sequence <= position) {
                pending_demands.push({
                    .deadline_sequence = _sequence_demands[demand_index].deadline_sequence,
                    .remaining_count   = _sequence_demands[demand_index].count,
                });
                ++demand_index;
            }
            if (pending_demands.empty()) {
                position = _sequence_demands[demand_index].release_sequence;
                continue;
            }
            if (pending_demands.top().deadline_sequence < position) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::CpuScopeTopologyInvalid,
                    "missing CpuScope parents cannot occupy distinct record-sequence holes"
                );
                return false;
            }

            while (observed_index < record_sequences.size() && record_sequences[observed_index] < position) {
                ++observed_index;
            }
            if (observed_index < record_sequences.size() && record_sequences[observed_index] == position) {
                ++position;
                ++observed_index;
                continue;
            }

            PendingCpuSequenceDemand demand = pending_demands.top();
            pending_demands.pop();
            std::uint64_t batch_end = demand.deadline_sequence + 1;
            if (observed_index < record_sequences.size()) {
                batch_end = std::min(batch_end, record_sequences[observed_index]);
            }
            if (demand_index < _sequence_demands.size()) {
                batch_end = std::min(batch_end, _sequence_demands[demand_index].release_sequence);
            }
            const std::uint64_t available = batch_end - position;
            const std::uint64_t assigned  = std::min(available, demand.remaining_count);
            position += assigned;
            demand.remaining_count -= assigned;
            if (demand.remaining_count != 0) {
                pending_demands.push(demand);
            }
        }
        return true;
    }

    [[nodiscard]] bool
    ValidateCpuLossCompatibility(std::span<const RecordSequenceDemand> _cpu_sequence_demands) {
        if (_cpu_sequence_demands.empty()) {
            return true;
        }

        struct LossBucket {
            std::uint64_t release_sequence{0};
            std::uint64_t deadline_sequence{0};
            std::uint64_t count{0};
        };

        std::uint64_t input_work = static_cast<std::uint64_t>(session.impl_->losses.size());
        if (AddOverflow(input_work, static_cast<std::uint64_t>(_cpu_sequence_demands.size()), input_work)) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "CPU/Loss topology input count overflows",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        if (!ChargeTopologyWork(input_work)) {
            return false;
        }

        std::uint64_t loss_bucket_count = 0;
        std::uint64_t loss_bucket_total = 0;
        for (const ProfileLossRecord& loss : session.impl_->losses) {
            std::uint64_t buckets_for_loss = 1;
            if (loss.record_count != 1) {
                ++buckets_for_loss;
            }
            if (loss.record_count > 2) {
                ++buckets_for_loss;
            }
            if (AddOverflow(loss_bucket_count, buckets_for_loss, loss_bucket_count) ||
                AddOverflow(loss_bucket_total, loss.record_count, loss_bucket_total)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::LossTotalsOverflow,
                    "profile noticed-loss bucket total overflows"
                );
                return false;
            }
        }
        std::uint64_t cpu_demand_total = 0;
        for (const RecordSequenceDemand& demand : _cpu_sequence_demands) {
            if (AddOverflow(cpu_demand_total, demand.count, cpu_demand_total)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::CpuScopeTopologyInvalid,
                    "CPU sequence-backed loss demand overflows"
                );
                return false;
            }
        }
        if (loss_bucket_total != session.impl_->summary.lost_record_count ||
            cpu_demand_total > loss_bucket_total) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::CpuScopeParentMissing,
                "CPU hierarchy deficits exceed noticed allocated-record losses"
            );
            return false;
        }

        std::uint64_t loss_critical_count   = 0;
        std::uint64_t demand_critical_count = 0;
        std::uint64_t critical_point_count  = 0;
        std::uint64_t construction_work     = 0;
        if (AddOverflow(loss_bucket_count, loss_bucket_count, loss_critical_count) ||
            AddOverflow(
                static_cast<std::uint64_t>(_cpu_sequence_demands.size()),
                static_cast<std::uint64_t>(_cpu_sequence_demands.size()),
                demand_critical_count
            ) ||
            AddOverflow(loss_critical_count, demand_critical_count, critical_point_count) ||
            AddOverflow(loss_bucket_count, critical_point_count, construction_work) ||
            loss_bucket_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            critical_point_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "CPU/Loss topology preprocessing exceeds the addressable range",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        if (!ChargeTopologyWork(construction_work)) {
            return false;
        }

        std::vector<LossBucket> loss_buckets;
        loss_buckets.reserve(static_cast<std::size_t>(loss_bucket_count));
        std::vector<std::uint64_t> critical_points;
        critical_points.reserve(static_cast<std::size_t>(critical_point_count));

        const auto add_loss_bucket = [&](std::uint64_t _release, std::uint64_t _deadline, std::uint64_t _count
                                     ) {
            if (_count == 0) {
                return;
            }
            loss_buckets.push_back({
                .release_sequence  = _release,
                .deadline_sequence = _deadline,
                .count             = _count,
            });
            critical_points.push_back(_release);
            critical_points.push_back(_deadline);
        };
        for (const ProfileLossRecord& loss : session.impl_->losses) {
            add_loss_bucket(loss.first_sequence, loss.first_sequence, 1);
            if (loss.record_count != 1) {
                add_loss_bucket(loss.last_sequence, loss.last_sequence, 1);
            }
            if (loss.record_count > 2) {
                add_loss_bucket(loss.first_sequence + 1, loss.last_sequence - 1, loss.record_count - 2);
            }
        }
        for (const RecordSequenceDemand& demand : _cpu_sequence_demands) {
            critical_points.push_back(demand.release_sequence);
            critical_points.push_back(demand.deadline_sequence);
        }

        // std::sort is introspective O(N log N). Charge one abstract work item
        // per element and merge level before invoking it so preprocessing cannot
        // escape the session-wide topology budget.
        std::uint64_t sort_width = 1;
        while (sort_width < critical_point_count) {
            if (!ChargeTopologyWork(critical_point_count)) {
                return false;
            }
            if (sort_width > std::numeric_limits<std::uint64_t>::max() / 2) {
                break;
            }
            sort_width *= 2;
        }
        std::sort(critical_points.begin(), critical_points.end());
        if (!ChargeTopologyWork(critical_point_count)) {
            return false;
        }
        critical_points.erase(
            std::unique(critical_points.begin(), critical_points.end()), critical_points.end()
        );

        struct SequenceSegment {
            std::uint64_t begin{0};
            std::uint64_t end{0};
            std::uint64_t capacity{0};
        };
        std::uint64_t maximum_segment_count = 0;
        if (!critical_points.empty()) {
            if (AddOverflow(
                    static_cast<std::uint64_t>(critical_points.size()),
                    static_cast<std::uint64_t>(critical_points.size()),
                    maximum_segment_count
                )) {
                Fail(
                    SessionLoadStatus::LimitExceeded,
                    SessionErrorCode::LimitExceeded,
                    "CPU/Loss sequence segmentation overflows",
                    DecodeStatus::LimitExceeded,
                    SessionLimitKind::TopologyWorkItems
                );
                return false;
            }
            --maximum_segment_count;
        }
        std::uint64_t segmentation_work = maximum_segment_count;
        if (AddOverflow(
                segmentation_work, static_cast<std::uint64_t>(record_sequences.size()), segmentation_work
            ) ||
            maximum_segment_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "CPU/Loss sequence segmentation exceeds the addressable range",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        if (!ChargeTopologyWork(segmentation_work)) {
            return false;
        }

        std::vector<SequenceSegment> segments;
        segments.reserve(static_cast<std::size_t>(maximum_segment_count));
        std::size_t observed_index = 0;
        const auto  add_segment    = [&](std::uint64_t _begin, std::uint64_t _end) {
            while (observed_index < record_sequences.size() && record_sequences[observed_index] < _begin) {
                ++observed_index;
            }
            const std::size_t observed_begin = observed_index;
            while (observed_index < record_sequences.size() && record_sequences[observed_index] <= _end) {
                ++observed_index;
            }
            const std::uint64_t observed_count = static_cast<std::uint64_t>(observed_index - observed_begin);
            const std::uint64_t span_minus_one = _end - _begin;
            std::uint64_t       available      = 0;
            if (span_minus_one == std::numeric_limits<std::uint64_t>::max()) {
                // The full uint64 domain has 2^64 values, which cannot itself
                // be represented. The available count is representable after
                // excluding any observed sequence, or can be capped directly
                // by the total flow when none are observed.
                available = observed_count == 0 ?
                                    loss_bucket_total :
                                    std::numeric_limits<std::uint64_t>::max() - (observed_count - 1);
            } else {
                const std::uint64_t span = span_minus_one + 1;
                available                = observed_count < span ? span - observed_count : 0;
            }
            const std::uint64_t capacity = std::min(available, loss_bucket_total);
            if (capacity != 0) {
                segments.push_back({
                        .begin    = _begin,
                        .end      = _end,
                        .capacity = capacity,
                });
            }
        };
        for (std::size_t index = 0; index < critical_points.size(); ++index) {
            const std::uint64_t point = critical_points[index];
            add_segment(point, point);
            if (index + 1 < critical_points.size() && point != std::numeric_limits<std::uint64_t>::max() &&
                critical_points[index + 1] > point + 1) {
                add_segment(point + 1, critical_points[index + 1] - 1);
            }
        }

        std::uint64_t graph_node_work = 3;
        const auto    add_node_work   = [&](std::uint64_t _count) {
            return !AddOverflow(graph_node_work, _count, graph_node_work);
        };
        if (!add_node_work(static_cast<std::uint64_t>(loss_buckets.size())) ||
            !add_node_work(static_cast<std::uint64_t>(segments.size())) ||
            !add_node_work(static_cast<std::uint64_t>(segments.size())) ||
            !add_node_work(static_cast<std::uint64_t>(_cpu_sequence_demands.size())) ||
            graph_node_work > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "CPU/Loss topology graph exceeds the addressable node range",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::TopologyWorkItems
            );
            return false;
        }
        if (!ChargeTopologyWork(graph_node_work)) {
            return false;
        }

        struct FlowNetwork {
            struct Edge {
                std::size_t   target{0};
                std::size_t   reverse_index{0};
                std::uint64_t capacity{0};
            };

            explicit FlowNetwork(Impl& _owner, std::size_t _node_count) :
                owner(&_owner),
                edges(_node_count),
                parent_nodes(_node_count),
                parent_edges(_node_count),
                pending_nodes(_node_count) {}

            void AddEdge(std::size_t _source, std::size_t _target, std::uint64_t _capacity) {
                const std::size_t source_reverse = edges[_target].size();
                const std::size_t target_reverse = edges[_source].size();
                edges[_source].push_back({
                    .target        = _target,
                    .reverse_index = source_reverse,
                    .capacity      = _capacity,
                });
                edges[_target].push_back({
                    .target        = _source,
                    .reverse_index = target_reverse,
                    .capacity      = 0,
                });
            }

            [[nodiscard]] bool
            MaxFlow(std::size_t _source, std::size_t _sink, std::uint64_t _target, std::uint64_t& _flow) {
                constexpr std::size_t unreached = std::numeric_limits<std::size_t>::max();
                _flow                           = 0;
                while (_flow < _target) {
                    if (!owner->ChargeTopologyWork(static_cast<std::uint64_t>(parent_nodes.size()))) {
                        return false;
                    }
                    std::fill(parent_nodes.begin(), parent_nodes.end(), unreached);
                    parent_nodes[_source]        = _source;
                    std::size_t pending_begin    = 0;
                    std::size_t pending_end      = 0;
                    pending_nodes[pending_end++] = _source;

                    while (pending_begin < pending_end && parent_nodes[_sink] == unreached) {
                        const std::size_t node = pending_nodes[pending_begin++];
                        if (!owner->ChargeTopologyWork(static_cast<std::uint64_t>(edges[node].size()))) {
                            return false;
                        }
                        for (std::size_t edge_index = 0; edge_index < edges[node].size(); ++edge_index) {
                            const Edge& edge = edges[node][edge_index];
                            if (edge.capacity != 0 && parent_nodes[edge.target] == unreached) {
                                parent_nodes[edge.target]    = node;
                                parent_edges[edge.target]    = edge_index;
                                pending_nodes[pending_end++] = edge.target;
                                if (edge.target == _sink) {
                                    break;
                                }
                            }
                        }
                    }
                    if (parent_nodes[_sink] == unreached) {
                        return true;
                    }

                    std::uint64_t augment = _target - _flow;
                    for (std::size_t node = _sink; node != _source; node = parent_nodes[node]) {
                        if (!owner->ChargeTopologyWork(1)) {
                            return false;
                        }
                        const Edge& edge = edges[parent_nodes[node]][parent_edges[node]];
                        augment          = std::min(augment, edge.capacity);
                    }

                    for (std::size_t node = _sink; node != _source; node = parent_nodes[node]) {
                        if (!owner->ChargeTopologyWork(1)) {
                            return false;
                        }
                        Edge& edge = edges[parent_nodes[node]][parent_edges[node]];
                        edge.capacity -= augment;
                        edges[node][edge.reverse_index].capacity += augment;
                    }
                    _flow += augment;
                }
                return true;
            }

            Impl*                          owner{nullptr};
            std::vector<std::vector<Edge>> edges;
            std::vector<std::size_t>       parent_nodes;
            std::vector<std::size_t>       parent_edges;
            std::vector<std::size_t>       pending_nodes;
        };

        const std::size_t source_node       = 0;
        const std::size_t loss_base         = 1;
        const std::size_t segment_in_base   = loss_base + loss_buckets.size();
        const std::size_t segment_out_base  = segment_in_base + segments.size();
        const std::size_t cpu_demand_base   = segment_out_base + segments.size();
        const std::size_t dummy_demand_node = cpu_demand_base + _cpu_sequence_demands.size();
        const std::size_t sink_node         = dummy_demand_node + 1;
        FlowNetwork       network(*this, sink_node + 1);
        const auto        add_edge = [&](std::size_t _source, std::size_t _target, std::uint64_t _capacity) {
            if (!ChargeTopologyWork(1)) {
                return false;
            }
            network.AddEdge(_source, _target, _capacity);
            return true;
        };

        for (std::size_t index = 0; index < loss_buckets.size(); ++index) {
            if (!add_edge(source_node, loss_base + index, loss_buckets[index].count)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < segments.size(); ++index) {
            if (!add_edge(segment_in_base + index, segment_out_base + index, segments[index].capacity)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < _cpu_sequence_demands.size(); ++index) {
            if (!add_edge(cpu_demand_base + index, sink_node, _cpu_sequence_demands[index].count)) {
                return false;
            }
        }
        if (!add_edge(dummy_demand_node, sink_node, loss_bucket_total - cpu_demand_total)) {
            return false;
        }

        const auto charge_segment_search = [&]() {
            std::uint64_t remaining = static_cast<std::uint64_t>(segments.size());
            std::uint64_t work      = 0;
            while (remaining != 0) {
                ++work;
                remaining /= 2;
            }
            return ChargeTopologyWork(work);
        };
        for (std::size_t loss_index = 0; loss_index < loss_buckets.size(); ++loss_index) {
            const LossBucket& bucket = loss_buckets[loss_index];
            if (!charge_segment_search()) {
                return false;
            }
            auto segment_it = std::lower_bound(
                segments.begin(),
                segments.end(),
                bucket.release_sequence,
                [](const SequenceSegment& _segment, std::uint64_t _sequence) {
                    return _segment.end < _sequence;
                }
            );
            for (; segment_it != segments.end() && segment_it->begin <= bucket.deadline_sequence;
                 ++segment_it) {
                if (!ChargeTopologyWork(1)) {
                    return false;
                }
                const std::size_t segment_index = static_cast<std::size_t>(segment_it - segments.begin());
                const SequenceSegment& segment  = segments[segment_index];
                if (segment.begin >= bucket.release_sequence && segment.end <= bucket.deadline_sequence &&
                    !add_edge(
                        loss_base + loss_index,
                        segment_in_base + segment_index,
                        std::min(bucket.count, segment.capacity)
                    )) {
                    return false;
                }
            }
        }
        for (std::size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
            if (!add_edge(
                    segment_out_base + segment_index, dummy_demand_node, segments[segment_index].capacity
                )) {
                return false;
            }
        }
        for (std::size_t demand_index = 0; demand_index < _cpu_sequence_demands.size(); ++demand_index) {
            const RecordSequenceDemand& demand = _cpu_sequence_demands[demand_index];
            if (!charge_segment_search()) {
                return false;
            }
            auto segment_it = std::lower_bound(
                segments.begin(),
                segments.end(),
                demand.release_sequence,
                [](const SequenceSegment& _segment, std::uint64_t _sequence) {
                    return _segment.end < _sequence;
                }
            );
            for (; segment_it != segments.end() && segment_it->begin <= demand.deadline_sequence;
                 ++segment_it) {
                if (!ChargeTopologyWork(1)) {
                    return false;
                }
                const std::size_t segment_index = static_cast<std::size_t>(segment_it - segments.begin());
                const SequenceSegment& segment  = segments[segment_index];
                if (segment.begin >= demand.release_sequence && segment.end <= demand.deadline_sequence &&
                    !add_edge(
                        segment_out_base + segment_index,
                        cpu_demand_base + demand_index,
                        std::min(segment.capacity, demand.count)
                    )) {
                    return false;
                }
            }
        }

        std::uint64_t allocated_loss_count = 0;
        if (!network.MaxFlow(source_node, sink_node, loss_bucket_total, allocated_loss_count)) {
            return false;
        }
        if (allocated_loss_count != loss_bucket_total) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::CpuScopeParentMissing,
                "noticed Loss intervals cannot jointly explain missing CPU parent sequences"
            );
            return false;
        }
        return true;
    }

    [[nodiscard]] bool BuildGpuDomainsAndTracks() {
        auto& frames = session.impl_->gpu_frames;
        std::sort(
            frames.begin(),
            frames.end(),
            [](const GpuFrameRecord& _left, const GpuFrameRecord& _right) {
                if (_left.frame_id != _right.frame_id) {
                    return _left.frame_id < _right.frame_id;
                }
                return _left.sequence < _right.sequence;
            }
        );
        for (std::size_t index = 1; index < frames.size(); ++index) {
            if (frames[index - 1].frame_id == frames[index].frame_id) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameDuplicate,
                    "duplicate GpuFrame identity"
                );
                return false;
            }
        }

        auto&                  scopes = session.impl_->gpu_scopes;
        std::vector<DomainKey> keys;
        keys.reserve(scopes.size());
        for (const GpuScopeRecord& scope : scopes) {
            keys.push_back({
                .native_queue_id = scope.native_queue_id,
                .family_id       = scope.family_id,
            });
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        if (keys.size() > options.limits.max_gpu_domains) {
            Fail(
                SessionLoadStatus::LimitExceeded,
                SessionErrorCode::LimitExceeded,
                "GPU timestamp domain limit exceeded",
                DecodeStatus::LimitExceeded,
                SessionLimitKind::GpuDomains
            );
            return false;
        }
        for (const DomainKey& key : keys) {
            if (!ChargeModel(sizeof(GpuTimestampDomain))) {
                return false;
            }
            session.impl_->gpu_domains.push_back({
                .native_queue_id = key.native_queue_id,
                .family_id       = key.family_id,
            });
        }

        for (GpuScopeRecord& scope : scopes) {
            const auto frame = std::lower_bound(
                frames.begin(),
                frames.end(),
                scope.frame_id,
                [](const GpuFrameRecord& _frame, std::uint64_t _frame_id) {
                    return _frame.frame_id < _frame_id;
                }
            );
            const bool timing_capability_trusted = frame != frames.end() &&
                                                   frame->frame_id == scope.frame_id &&
                                                   frame->status == ProfileGpuFrameStatus::Complete;
            const DomainKey key{
                .native_queue_id = scope.native_queue_id,
                .family_id       = scope.family_id,
            };
            const auto          domain_position = std::lower_bound(keys.begin(), keys.end(), key);
            const std::uint32_t domain_index    = static_cast<std::uint32_t>(domain_position - keys.begin());
            scope.domain_index                  = domain_index;
            GpuTimestampDomain& domain          = session.impl_->gpu_domains[domain_index];
            domain.logical_queue_mask |= ProfileLogicalQueueBit(scope.logical_queue);
            if (scope.status == ProfileGpuScopeStatus::Ready) {
                ++domain.ready_scope_count;
                if (timing_capability_trusted && !domain.timing_capability_trusted) {
                    domain.has_ready_timestamps      = true;
                    domain.timing_capability_trusted = true;
                    domain.valid_bits                = scope.valid_bits;
                    domain.tick_period_ns            = scope.tick_period_ns;
                } else if (timing_capability_trusted &&
                           (domain.valid_bits != scope.valid_bits ||
                            !NearlyEqualDuration(domain.tick_period_ns, scope.tick_period_ns))) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuDomainConflict,
                        "trusted GPU scopes report conflicting timing capabilities"
                    );
                    return false;
                } else if (!domain.has_ready_timestamps) {
                    domain.has_ready_timestamps = true;
                    domain.valid_bits           = scope.valid_bits;
                    domain.tick_period_ns       = scope.tick_period_ns;
                }
            } else {
                ++domain.error_scope_count;
            }
        }

        std::sort(
            scopes.begin(),
            scopes.end(),
            [](const GpuScopeRecord& _left, const GpuScopeRecord& _right) {
                if (_left.logical_queue != _right.logical_queue) {
                    return _left.logical_queue < _right.logical_queue;
                }
                if (_left.native_queue_id != _right.native_queue_id) {
                    return _left.native_queue_id < _right.native_queue_id;
                }
                if (_left.family_id != _right.family_id) {
                    return _left.family_id < _right.family_id;
                }
                if (_left.frame_id != _right.frame_id) {
                    return _left.frame_id < _right.frame_id;
                }
                if (_left.source_order != _right.source_order) {
                    return _left.source_order < _right.source_order;
                }
                if (_left.local_order != _right.local_order) {
                    return _left.local_order < _right.local_order;
                }
                return _left.scope_id < _right.scope_id;
            }
        );

        std::size_t track_begin = 0;
        while (track_begin < scopes.size()) {
            std::size_t track_end = track_begin + 1;
            while (track_end < scopes.size() &&
                   scopes[track_end].logical_queue == scopes[track_begin].logical_queue &&
                   scopes[track_end].native_queue_id == scopes[track_begin].native_queue_id &&
                   scopes[track_end].family_id == scopes[track_begin].family_id) {
                ++track_end;
            }
            if (!CheckCountLimit(
                    session.impl_->gpu_tracks.size(),
                    options.limits.max_gpu_tracks,
                    SessionLimitKind::GpuTracks,
                    "GPU track limit exceeded"
                ) ||
                !ChargeModel(sizeof(GpuTrack))) {
                return false;
            }
            const std::uint32_t track_index = static_cast<std::uint32_t>(session.impl_->gpu_tracks.size());
            session.impl_->gpu_tracks.push_back({
                .logical_queue   = scopes[track_begin].logical_queue,
                .native_queue_id = scopes[track_begin].native_queue_id,
                .family_id       = scopes[track_begin].family_id,
                .domain_index    = scopes[track_begin].domain_index,
                .first_scope     = track_begin,
                .scope_count     = track_end - track_begin,
            });
            for (std::size_t index = track_begin; index < track_end; ++index) {
                scopes[index].track_index = track_index;
            }
            track_begin = track_end;
        }
        return true;
    }

    [[nodiscard]] bool BuildGpuTopology(bool _allow_missing, std::uint64_t _required_cpu_drops) {
        auto& frames = session.impl_->gpu_frames;
        auto& scopes = session.impl_->gpu_scopes;

        const auto charge_topology_work = [&](std::uint64_t _count) {
            return ChargeTopologyWork(_count);
        };
        std::vector<std::uint64_t> frame_error_counts(frames.size(), 0);
        std::vector<std::uint8_t>  frame_root_partition_ambiguous(frames.size(), 0);
        std::vector<ScopeLookup>   lookup;
        lookup.reserve(scopes.size());
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            lookup.push_back({
                .frame_id    = scopes[index].frame_id,
                .scope_id    = scopes[index].scope_id,
                .scope_index = index,
            });
        }
        std::sort(lookup.begin(), lookup.end());
        for (std::size_t index = 1; index < lookup.size(); ++index) {
            if (lookup[index - 1].frame_id == lookup[index].frame_id &&
                lookup[index - 1].scope_id == lookup[index].scope_id) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeDuplicate,
                    "duplicate GpuScope identity within one frame"
                );
                return false;
            }
        }

        const auto find_scope = [&](std::uint64_t _frame_id, std::uint64_t _scope_id) {
            const ScopeLookup key{
                .frame_id    = _frame_id,
                .scope_id    = _scope_id,
                .scope_index = 0,
            };
            const auto found = std::lower_bound(lookup.begin(), lookup.end(), key);
            return found != lookup.end() && found->frame_id == _frame_id && found->scope_id == _scope_id ?
                       found->scope_index :
                       kInvalidSessionIndex;
        };

        std::vector<std::uint64_t> previous_child_end(scopes.size(), 0);
        std::vector<std::uint8_t>  has_previous_child(scopes.size(), 0);
        std::vector<double>        direct_child_duration(scopes.size(), 0.0);
        std::vector<ScopeLookup>   missing_parent_keys;
        std::vector<std::uint64_t> missing_frame_ids;
        struct MissingParentEvidence {
            std::uint64_t       frame_id{0};
            std::uint64_t       parent_scope_id{0};
            ProfileLogicalQueue logical_queue{ProfileLogicalQueue::Graphics};
            std::uint32_t       native_queue_id{0};
            std::uint32_t       family_id{0};
            std::uint64_t       source_order{0};
            std::uint32_t       parent_depth{0};
            std::uint64_t       child_local_order{0};
            std::uint64_t       child_index{0};
        };
        std::vector<MissingParentEvidence> missing_parent_evidence;
        std::vector<std::uint64_t>         source_local_rank(scopes.size(), 0);
        std::vector<std::uint64_t> nearest_observed_timing_ancestor(scopes.size(), kInvalidSessionIndex);
        std::vector<std::uint64_t> completed_observed_child_end(scopes.size(), 0);
        std::vector<std::uint64_t> active_timing_stack_position(scopes.size(), kInvalidSessionIndex);
        missing_parent_keys.reserve(scopes.size());
        missing_parent_evidence.reserve(scopes.size());
        missing_frame_ids.reserve(scopes.size());

        for (std::size_t index = 1; index < scopes.size(); ++index) {
            const GpuScopeRecord& previous = scopes[index - 1];
            const GpuScopeRecord& scope    = scopes[index];
            const bool            same_source =
                previous.logical_queue == scope.logical_queue &&
                previous.native_queue_id == scope.native_queue_id && previous.family_id == scope.family_id &&
                previous.frame_id == scope.frame_id && previous.source_order == scope.source_order;
            if (same_source && previous.local_order == scope.local_order) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeIdentityInvalid,
                    "GpuScope local order is duplicated within one recording source"
                );
                return false;
            }
            if (same_source) {
                source_local_rank[index] = source_local_rank[index - 1] + 1;
            }
        }

        for (std::size_t index = 0; index < scopes.size(); ++index) {
            GpuScopeRecord& scope  = scopes[index];
            bool            orphan = false;

            const auto frame = std::lower_bound(
                frames.begin(),
                frames.end(),
                scope.frame_id,
                [](const GpuFrameRecord& _frame, std::uint64_t _frame_id) {
                    return _frame.frame_id < _frame_id;
                }
            );
            bool validate_timing = false;
            if (frame == frames.end() || frame->frame_id != scope.frame_id) {
                orphan = true;
                missing_frame_ids.push_back(scope.frame_id);
                if (!_allow_missing) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeFrameMissing,
                        "GpuScope references a missing GpuFrame"
                    );
                    return false;
                }
            } else {
                const std::size_t frame_index = static_cast<std::size_t>(frame - frames.begin());
                scope.frame_index             = frame_index;
                // GpuFrame v1 only proves that producer-side topology was
                // valid for Complete. Incomplete can also represent a
                // structurally invalid frame accompanied by RHI drops.
                validate_timing = frame->status == ProfileGpuFrameStatus::Complete;
                ++frame->scope_count;
                if (scope.status == ProfileGpuScopeStatus::Error) {
                    ++frame_error_counts[frame_index];
                }
            }

            if (scope.parent_scope_id == 0) {
                if (scope.depth != 0) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeRootDepthInvalid,
                        "root GpuScope has a non-zero depth"
                    );
                    return false;
                }
                if (validate_timing) {
                    if (scope.status != ProfileGpuScopeStatus::Ready) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "non-invalid GpuFrame contains a non-ready root scope"
                        );
                        return false;
                    }
                }
            } else {
                if (scope.depth == 0) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "non-root GpuScope has zero depth"
                    );
                    return false;
                }
                const std::uint64_t parent_index = find_scope(scope.frame_id, scope.parent_scope_id);
                if (parent_index == kInvalidSessionIndex) {
                    orphan = true;
                    missing_parent_keys.push_back({
                        .frame_id    = scope.frame_id,
                        .scope_id    = scope.parent_scope_id,
                        .scope_index = 0,
                    });
                    missing_parent_evidence.push_back({
                        .frame_id          = scope.frame_id,
                        .parent_scope_id   = scope.parent_scope_id,
                        .logical_queue     = scope.logical_queue,
                        .native_queue_id   = scope.native_queue_id,
                        .family_id         = scope.family_id,
                        .source_order      = scope.source_order,
                        .parent_depth      = scope.depth - 1,
                        .child_local_order = scope.local_order,
                        .child_index       = index,
                    });
                    if (!_allow_missing) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentMissing,
                            "GpuScope references a missing parent"
                        );
                        return false;
                    }
                } else {
                    const GpuScopeRecord& parent = scopes[parent_index];
                    if (parent.logical_queue != scope.logical_queue ||
                        parent.native_queue_id != scope.native_queue_id ||
                        parent.family_id != scope.family_id || parent.source_order != scope.source_order ||
                        parent.depth != scope.depth - 1 || parent.local_order >= scope.local_order) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "GpuScope parent topology is inconsistent"
                        );
                        return false;
                    }
                    scope.parent_index = parent_index;
                    if (validate_timing) {
                        if (parent.status != ProfileGpuScopeStatus::Ready ||
                            scope.status != ProfileGpuScopeStatus::Ready ||
                            parent.valid_bits != scope.valid_bits ||
                            !NearlyEqualDuration(parent.tick_period_ns, scope.tick_period_ns)) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeTimingInvalid,
                                "non-invalid GpuFrame has incompatible parent/child timing metadata"
                            );
                            return false;
                        }

                        const std::uint64_t parent_delta =
                            TimestampDelta(parent.begin_tick, parent.end_tick, parent.valid_bits);
                        const std::uint64_t child_begin_offset =
                            (scope.begin_tick - parent.begin_tick) & TimestampMask(parent.valid_bits);
                        const std::uint64_t child_delta =
                            TimestampDelta(scope.begin_tick, scope.end_tick, scope.valid_bits);
                        const bool contained = child_begin_offset <= parent_delta &&
                                               child_delta <= parent_delta - child_begin_offset;
                        const bool ordered = has_previous_child[parent_index] == 0 ||
                                             child_begin_offset >= previous_child_end[parent_index];
                        if (!contained || !ordered) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeTimingInvalid,
                                "GpuScope children are outside their parent or overlap"
                            );
                            return false;
                        }
                        previous_child_end[parent_index] = child_begin_offset + child_delta;
                        has_previous_child[parent_index] = 1;
                        direct_child_duration[parent_index] += scope.total_duration_ns;
                    }
                }
            }
            if (orphan) {
                ++session.impl_->summary.orphan_gpu_scope_count;
            }
        }

        std::sort(
            missing_parent_evidence.begin(),
            missing_parent_evidence.end(),
            [](const MissingParentEvidence& _left, const MissingParentEvidence& _right) {
                if (_left.frame_id != _right.frame_id) {
                    return _left.frame_id < _right.frame_id;
                }
                if (_left.parent_scope_id != _right.parent_scope_id) {
                    return _left.parent_scope_id < _right.parent_scope_id;
                }
                return _left.child_local_order < _right.child_local_order;
            }
        );
        struct TimingContractEnvelope {
            std::uint32_t parent_depth{0};
            std::uint64_t begin_tick{0};
            std::uint64_t duration{0};
            bool          trusted{false};
        };
        std::vector<TimingContractEnvelope> timing_contract_envelopes;
        std::vector<std::uint64_t>          timing_contract_for_child(scopes.size(), kInvalidSessionIndex);
        timing_contract_envelopes.reserve(missing_parent_evidence.size());
        std::size_t timing_evidence_begin = 0;
        while (timing_evidence_begin < missing_parent_evidence.size()) {
            std::size_t timing_evidence_end = timing_evidence_begin + 1;
            while (timing_evidence_end < missing_parent_evidence.size() &&
                   missing_parent_evidence[timing_evidence_end].frame_id ==
                       missing_parent_evidence[timing_evidence_begin].frame_id &&
                   missing_parent_evidence[timing_evidence_end].parent_scope_id ==
                       missing_parent_evidence[timing_evidence_begin].parent_scope_id) {
                ++timing_evidence_end;
            }

            const MissingParentEvidence& first_evidence = missing_parent_evidence[timing_evidence_begin];
            const GpuScopeRecord& first_child = scopes[static_cast<std::size_t>(first_evidence.child_index)];
            const auto            frame       = std::lower_bound(
                frames.begin(),
                frames.end(),
                first_evidence.frame_id,
                [](const GpuFrameRecord& _frame, std::uint64_t _frame_id) {
                    return _frame.frame_id < _frame_id;
                }
            );
            const bool trusted = frame != frames.end() && frame->frame_id == first_evidence.frame_id &&
                                 frame->status == ProfileGpuFrameStatus::Complete;
            std::uint64_t envelope_end_offset = 0;
            for (std::size_t index = timing_evidence_begin; index < timing_evidence_end; ++index) {
                const MissingParentEvidence& evidence = missing_parent_evidence[index];
                const GpuScopeRecord&        child = scopes[static_cast<std::size_t>(evidence.child_index)];
                if (evidence.logical_queue != first_evidence.logical_queue ||
                    evidence.native_queue_id != first_evidence.native_queue_id ||
                    evidence.family_id != first_evidence.family_id ||
                    evidence.source_order != first_evidence.source_order ||
                    evidence.parent_depth != first_evidence.parent_depth) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "children disagree on the contract of one missing GpuScope parent"
                    );
                    return false;
                }
                if (trusted) {
                    if (child.status != ProfileGpuScopeStatus::Ready ||
                        child.valid_bits != first_child.valid_bits ||
                        !NearlyEqualDuration(child.tick_period_ns, first_child.tick_period_ns)) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "children of one missing GPU parent disagree on timestamp metadata"
                        );
                        return false;
                    }
                    const std::uint64_t child_begin_offset =
                        (child.begin_tick - first_child.begin_tick) & TimestampMask(first_child.valid_bits);
                    const std::uint64_t child_duration =
                        TimestampDelta(child.begin_tick, child.end_tick, child.valid_bits);
                    const std::uint64_t timestamp_mask = TimestampMask(first_child.valid_bits);
                    if (child_begin_offset < envelope_end_offset ||
                        child_duration > timestamp_mask - child_begin_offset) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "children of one missing parent do not form one ordered timestamp envelope"
                        );
                        return false;
                    }
                    envelope_end_offset = child_begin_offset + child_duration;
                }
            }

            const std::uint64_t contract_index = static_cast<std::uint64_t>(timing_contract_envelopes.size());
            timing_contract_envelopes.push_back({
                .parent_depth = first_evidence.parent_depth,
                .begin_tick   = first_child.begin_tick,
                .duration     = envelope_end_offset,
                .trusted      = trusted,
            });
            for (std::size_t index = timing_evidence_begin; index < timing_evidence_end; ++index) {
                timing_contract_for_child[static_cast<std::size_t>(missing_parent_evidence[index].child_index
                )] = contract_index;
            }
            timing_evidence_begin = timing_evidence_end;
        }

        std::size_t timing_source_begin = 0;
        while (timing_source_begin < scopes.size()) {
            std::size_t timing_source_end = timing_source_begin + 1;
            while (timing_source_end < scopes.size()) {
                const GpuScopeRecord& first = scopes[timing_source_begin];
                const GpuScopeRecord& next  = scopes[timing_source_end];
                if (first.logical_queue != next.logical_queue ||
                    first.native_queue_id != next.native_queue_id || first.family_id != next.family_id ||
                    first.frame_id != next.frame_id || first.source_order != next.source_order) {
                    break;
                }
                ++timing_source_end;
            }

            const GpuScopeRecord& first_scope = scopes[timing_source_begin];
            const bool            timing_topology_trusted =
                first_scope.frame_index != kInvalidSessionIndex &&
                frames[first_scope.frame_index].status == ProfileGpuFrameStatus::Complete;
            if (timing_topology_trusted) {
                std::vector<std::uint64_t> timing_stack;
                timing_stack.reserve(std::min<std::size_t>(
                    timing_source_end - timing_source_begin,
                    static_cast<std::size_t>(options.limits.max_scope_depth) + 1
                ));
                const auto close_observed_subtrees = [&](std::size_t _keep_count) {
                    while (timing_stack.size() > _keep_count) {
                        const std::size_t child_index = static_cast<std::size_t>(timing_stack.back());
                        timing_stack.pop_back();
                        active_timing_stack_position[child_index] = kInvalidSessionIndex;
                        if (timing_stack.empty()) {
                            continue;
                        }
                        const std::size_t     parent_index = static_cast<std::size_t>(timing_stack.back());
                        const GpuScopeRecord& parent       = scopes[parent_index];
                        const GpuScopeRecord& child        = scopes[child_index];
                        const std::uint64_t   child_begin_offset =
                            (child.begin_tick - parent.begin_tick) & TimestampMask(parent.valid_bits);
                        const std::uint64_t child_duration =
                            TimestampDelta(child.begin_tick, child.end_tick, child.valid_bits);
                        completed_observed_child_end[parent_index] = std::max(
                            completed_observed_child_end[parent_index], child_begin_offset + child_duration
                        );
                    }
                };
                for (std::size_t index = timing_source_begin; index < timing_source_end; ++index) {
                    const GpuScopeRecord& scope = scopes[index];
                    if (scope.status != ProfileGpuScopeStatus::Ready) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "Complete GpuFrame contains a non-ready scope"
                        );
                        return false;
                    }
                    if (first_scope.valid_bits != scope.valid_bits ||
                        !NearlyEqualDuration(first_scope.tick_period_ns, scope.tick_period_ns)) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "Complete GPU source changes timestamp metadata"
                        );
                        return false;
                    }

                    std::size_t container_position = timing_stack.size();
                    if (scope.parent_index != kInvalidSessionIndex) {
                        const std::uint64_t active_position =
                            active_timing_stack_position[scope.parent_index];
                        if (active_position == kInvalidSessionIndex ||
                            active_position >= timing_stack.size() ||
                            timing_stack[static_cast<std::size_t>(active_position)] != scope.parent_index) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeParentInvalid,
                                "observed GPU parent was already closed by a later subtree"
                            );
                            return false;
                        }
                        container_position = static_cast<std::size_t>(active_position);
                    } else if (scope.parent_scope_id != 0) {
                        const std::uint64_t timing_contract_index = timing_contract_for_child[index];
                        const TimingContractEnvelope* timing_contract =
                            timing_contract_index == kInvalidSessionIndex ?
                                nullptr :
                                &timing_contract_envelopes[static_cast<std::size_t>(timing_contract_index)];
                        const std::uint32_t required_parent_depth =
                            timing_contract != nullptr ? timing_contract->parent_depth : scope.depth - 1;
                        const std::uint64_t required_begin_tick =
                            timing_contract != nullptr && timing_contract->trusted ?
                                timing_contract->begin_tick :
                                scope.begin_tick;
                        const std::uint64_t required_duration =
                            timing_contract != nullptr && timing_contract->trusted ?
                                timing_contract->duration :
                                TimestampDelta(scope.begin_tick, scope.end_tick, scope.valid_bits);
                        for (std::size_t position = timing_stack.size(); position != 0; --position) {
                            if (!charge_topology_work(1)) {
                                return false;
                            }
                            const GpuScopeRecord& candidate = scopes[timing_stack[position - 1]];
                            if (candidate.depth >= required_parent_depth) {
                                continue;
                            }
                            const std::uint64_t candidate_duration = TimestampDelta(
                                candidate.begin_tick, candidate.end_tick, candidate.valid_bits
                            );
                            const std::uint64_t required_begin_offset =
                                (required_begin_tick - candidate.begin_tick) &
                                TimestampMask(candidate.valid_bits);
                            if (required_begin_offset <= candidate_duration &&
                                required_duration <= candidate_duration - required_begin_offset) {
                                container_position = position - 1;
                                break;
                            }
                        }
                    }

                    if (container_position != timing_stack.size()) {
                        close_observed_subtrees(container_position + 1);
                        const std::size_t     container_index = static_cast<std::size_t>(timing_stack.back());
                        const GpuScopeRecord& container       = scopes[container_index];
                        const std::uint64_t   scope_begin_offset =
                            (scope.begin_tick - container.begin_tick) & TimestampMask(container.valid_bits);
                        const std::uint64_t scope_duration =
                            TimestampDelta(scope.begin_tick, scope.end_tick, scope.valid_bits);
                        const std::uint64_t container_duration =
                            TimestampDelta(container.begin_tick, container.end_tick, container.valid_bits);
                        if (scope_begin_offset > container_duration ||
                            scope_duration > container_duration - scope_begin_offset ||
                            scope_begin_offset < completed_observed_child_end[container_index]) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeTimingInvalid,
                                "Complete GPU source has crossing observed subtrees"
                            );
                            return false;
                        }
                        nearest_observed_timing_ancestor[index] = container_index;
                    } else {
                        if (scope.parent_index != kInvalidSessionIndex) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeTimingInvalid,
                                "observed GPU parent does not contain its child"
                            );
                            return false;
                        }
                        if (scope.parent_scope_id == 0 && !timing_stack.empty()) {
                            const GpuScopeRecord& previous_root = scopes[timing_stack.front()];
                            if (previous_root.parent_scope_id == 0) {
                                const std::uint64_t previous_duration = TimestampDelta(
                                    previous_root.begin_tick, previous_root.end_tick, previous_root.valid_bits
                                );
                                const std::uint64_t begin_offset =
                                    (scope.begin_tick - previous_root.begin_tick) &
                                    TimestampMask(scope.valid_bits);
                                const std::uint64_t missing_local_orders =
                                    index == timing_source_begin ?
                                        0 :
                                        scope.local_order - scopes[index - 1].local_order - 1;
                                if (!CanBridgeSerialTimestampGap(
                                        previous_duration,
                                        begin_offset,
                                        scope.valid_bits,
                                        missing_local_orders
                                    )) {
                                    Fail(
                                        SessionLoadStatus::ProtocolViolation,
                                        SessionErrorCode::GpuScopeTimingInvalid,
                                        "Complete GPU roots overlap or move backward"
                                    );
                                    return false;
                                }
                            }
                        }
                        close_observed_subtrees(0);
                    }

                    const std::uint64_t timing_parent = nearest_observed_timing_ancestor[index];
                    if (scope.parent_index != kInvalidSessionIndex && timing_parent != scope.parent_index) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "observed GPU parent does not match the timing hierarchy"
                        );
                        return false;
                    }
                    if (scope.parent_scope_id == 0 && timing_parent != kInvalidSessionIndex) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "GPU root is nested inside an observed interval"
                        );
                        return false;
                    }
                    if (scope.parent_scope_id != 0 && scope.parent_index == kInvalidSessionIndex &&
                        timing_parent != kInvalidSessionIndex &&
                        scopes[timing_parent].depth >= scope.depth - 1) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "missing GPU parent conflicts with an observed timing ancestor"
                        );
                        return false;
                    }
                    timing_stack.push_back(index);
                    active_timing_stack_position[index] = timing_stack.size() - 1;
                }
                close_observed_subtrees(0);
            }
            timing_source_begin = timing_source_end;
        }
        std::vector<std::uint64_t> timing_subtree_end_local(scopes.size(), 0);
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            timing_subtree_end_local[index] = scopes[index].local_order;
        }
        for (std::size_t index = scopes.size(); index != 0; --index) {
            const std::size_t   scope_index  = index - 1;
            const std::uint64_t parent_index = nearest_observed_timing_ancestor[scope_index];
            if (parent_index != kInvalidSessionIndex) {
                timing_subtree_end_local[parent_index] =
                    std::max(timing_subtree_end_local[parent_index], timing_subtree_end_local[scope_index]);
            }
        }
        std::size_t depth_range_leaf_count = 1;
        while (depth_range_leaf_count < scopes.size()) {
            depth_range_leaf_count *= 2;
        }
        std::vector<std::uint32_t> depth_range_tree(
            depth_range_leaf_count * 2, std::numeric_limits<std::uint32_t>::max()
        );
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            depth_range_tree[depth_range_leaf_count + index] = scopes[index].depth;
        }
        for (std::size_t index = depth_range_leaf_count; index != 1; --index) {
            const std::size_t parent = index - 1;
            depth_range_tree[parent] =
                std::min(depth_range_tree[parent * 2], depth_range_tree[parent * 2 + 1]);
        }
        const auto minimum_depth_in_range = [&](std::size_t _begin, std::size_t _end) {
            std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
            std::size_t   begin   = depth_range_leaf_count + _begin;
            std::size_t   end     = depth_range_leaf_count + _end;
            while (begin < end) {
                if ((begin & 1) != 0) {
                    minimum = std::min(minimum, depth_range_tree[begin++]);
                }
                if ((end & 1) != 0) {
                    minimum = std::min(minimum, depth_range_tree[--end]);
                }
                begin /= 2;
                end /= 2;
            }
            return minimum;
        };

        std::sort(
            missing_parent_evidence.begin(),
            missing_parent_evidence.end(),
            [](const MissingParentEvidence& _left, const MissingParentEvidence& _right) {
                if (_left.frame_id != _right.frame_id) {
                    return _left.frame_id < _right.frame_id;
                }
                if (_left.parent_scope_id != _right.parent_scope_id) {
                    return _left.parent_scope_id < _right.parent_scope_id;
                }
                return _left.child_local_order < _right.child_local_order;
            }
        );
        struct MissingParentContract {
            std::uint64_t       frame_id{0};
            ProfileLogicalQueue logical_queue{ProfileLogicalQueue::Graphics};
            std::uint32_t       native_queue_id{0};
            std::uint32_t       family_id{0};
            std::uint64_t       source_order{0};
            std::uint32_t       parent_depth{0};
            std::uint64_t       child_local_order_deadline{0};
            std::uint64_t       latest_parent_local_order{0};
            std::uint64_t       observed_local_orders_before_deadline{0};
            bool                timing_topology_trusted{false};
            std::uint64_t       observed_ancestor_index{kInvalidSessionIndex};
            std::uint64_t       last_child_subtree_end_local{0};
            std::uint64_t       timing_envelope_begin_tick{0};
            std::uint64_t       timing_envelope_duration{0};
        };
        std::vector<MissingParentContract> missing_parent_contracts;
        missing_parent_contracts.reserve(missing_parent_evidence.size());

        std::size_t evidence_begin = 0;
        while (evidence_begin < missing_parent_evidence.size()) {
            std::size_t evidence_end = evidence_begin + 1;
            while (evidence_end < missing_parent_evidence.size() &&
                   missing_parent_evidence[evidence_end].frame_id ==
                       missing_parent_evidence[evidence_begin].frame_id &&
                   missing_parent_evidence[evidence_end].parent_scope_id ==
                       missing_parent_evidence[evidence_begin].parent_scope_id) {
                ++evidence_end;
            }

            const MissingParentEvidence& contract = missing_parent_evidence[evidence_begin];
            std::uint64_t                last_child_subtree_end_local =
                timing_subtree_end_local[static_cast<std::size_t>(contract.child_index)];
            for (std::size_t index = evidence_begin + 1; index < evidence_end; ++index) {
                const MissingParentEvidence& evidence = missing_parent_evidence[index];
                if (evidence.logical_queue != contract.logical_queue ||
                    evidence.native_queue_id != contract.native_queue_id ||
                    evidence.family_id != contract.family_id ||
                    evidence.source_order != contract.source_order ||
                    evidence.parent_depth != contract.parent_depth) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "children disagree on the contract of one missing GpuScope parent"
                    );
                    return false;
                }
                last_child_subtree_end_local = std::max(
                    last_child_subtree_end_local,
                    timing_subtree_end_local[static_cast<std::size_t>(evidence.child_index)]
                );
            }

            const std::size_t   first_child_index = static_cast<std::size_t>(contract.child_index);
            const std::uint64_t observed_before   = source_local_rank[first_child_index];
            if (contract.child_local_order == 0 || contract.child_local_order <= observed_before) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeParentInvalid,
                    "a missing GpuScope parent has no earlier local-order slot"
                );
                return false;
            }
            std::uint64_t latest_parent_local_order = contract.child_local_order - 1;
            if (observed_before != 0) {
                const std::size_t last_observed_index = first_child_index - 1;
                if (scopes[last_observed_index].local_order == latest_parent_local_order) {
                    const std::size_t source_begin_index =
                        first_child_index - static_cast<std::size_t>(observed_before);
                    const std::uint64_t contiguous_run_key =
                        scopes[last_observed_index].local_order - (observed_before - 1);
                    std::size_t low  = source_begin_index;
                    std::size_t high = first_child_index;
                    while (low < high) {
                        const std::size_t   mid = low + (high - low) / 2;
                        const std::uint64_t order_without_rank =
                            scopes[mid].local_order - static_cast<std::uint64_t>(mid - source_begin_index);
                        if (order_without_rank < contiguous_run_key) {
                            low = mid + 1;
                        } else {
                            high = mid;
                        }
                    }
                    if (low == first_child_index || scopes[low].local_order == 0) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "a missing GpuScope parent has no unobserved local-order slot"
                        );
                        return false;
                    }
                    latest_parent_local_order = scopes[low].local_order - 1;
                }
            }

            const auto frame = std::lower_bound(
                frames.begin(),
                frames.end(),
                contract.frame_id,
                [](const GpuFrameRecord& _frame, std::uint64_t _frame_id) {
                    return _frame.frame_id < _frame_id;
                }
            );
            const bool timing_topology_trusted = frame != frames.end() &&
                                                 frame->frame_id == contract.frame_id &&
                                                 frame->status == ProfileGpuFrameStatus::Complete;
            std::uint64_t       observed_ancestor_index = kInvalidSessionIndex;
            const std::uint64_t timing_contract_index   = timing_contract_for_child[first_child_index];
            const TimingContractEnvelope* timing_contract =
                timing_contract_index == kInvalidSessionIndex ?
                    nullptr :
                    &timing_contract_envelopes[static_cast<std::size_t>(timing_contract_index)];
            std::uint64_t envelope_begin_tick = timing_contract != nullptr ?
                                                    timing_contract->begin_tick :
                                                    scopes[first_child_index].begin_tick;
            std::uint64_t envelope_end_offset = timing_contract != nullptr ?
                                                    timing_contract->duration :
                                                    TimestampDelta(
                                                        scopes[first_child_index].begin_tick,
                                                        scopes[first_child_index].end_tick,
                                                        scopes[first_child_index].valid_bits
                                                    );
            if (timing_topology_trusted) {
                const GpuScopeRecord& first_child =
                    scopes[missing_parent_evidence[evidence_begin].child_index];
                envelope_end_offset = 0;
                for (std::size_t index = evidence_begin; index < evidence_end; ++index) {
                    const GpuScopeRecord& child = scopes[missing_parent_evidence[index].child_index];
                    if (child.status != ProfileGpuScopeStatus::Ready) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "Complete GpuFrame has a non-ready child of a missing parent"
                        );
                        return false;
                    }
                    if (first_child.valid_bits != child.valid_bits ||
                        !NearlyEqualDuration(first_child.tick_period_ns, child.tick_period_ns)) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "children of one missing parent disagree on timestamp metadata"
                        );
                        return false;
                    }
                    const std::uint64_t child_begin_offset =
                        (child.begin_tick - envelope_begin_tick) & TimestampMask(child.valid_bits);
                    const std::uint64_t child_duration =
                        TimestampDelta(child.begin_tick, child.end_tick, child.valid_bits);
                    const std::uint64_t timestamp_mask = TimestampMask(child.valid_bits);
                    if (child_begin_offset < envelope_end_offset ||
                        child_duration > timestamp_mask - child_begin_offset) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "children of one missing parent do not form one ordered timestamp envelope"
                        );
                        return false;
                    }
                    envelope_end_offset = child_begin_offset + child_duration;
                    const std::uint64_t child_observed_ancestor =
                        nearest_observed_timing_ancestor[missing_parent_evidence[index].child_index];
                    if (index == evidence_begin) {
                        observed_ancestor_index = child_observed_ancestor;
                    } else if (child_observed_ancestor != observed_ancestor_index) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "children of one missing GPU parent disagree on their observed timing ancestor"
                        );
                        return false;
                    }
                }
                if (observed_ancestor_index != kInvalidSessionIndex &&
                    scopes[observed_ancestor_index].local_order >= latest_parent_local_order) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "a missing GPU parent has no slot after its observed timing ancestor"
                    );
                    return false;
                }
                if (observed_ancestor_index != kInvalidSessionIndex) {
                    const GpuScopeRecord& ancestor = scopes[observed_ancestor_index];
                    const std::uint64_t   ancestor_duration =
                        TimestampDelta(ancestor.begin_tick, ancestor.end_tick, ancestor.valid_bits);
                    const std::uint64_t envelope_begin_offset =
                        (envelope_begin_tick - ancestor.begin_tick) & TimestampMask(ancestor.valid_bits);
                    if (envelope_begin_offset > ancestor_duration ||
                        envelope_end_offset > ancestor_duration - envelope_begin_offset) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "observed GPU ancestor does not contain the missing-parent child envelope"
                        );
                        return false;
                    }
                }
                const std::size_t last_child_index =
                    static_cast<std::size_t>(missing_parent_evidence[evidence_end - 1].child_index);
                if (minimum_depth_in_range(first_child_index, last_child_index + 1) <=
                    contract.parent_depth) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "children of one missing GPU parent cross an observed hierarchy barrier"
                    );
                    return false;
                }
            }

            missing_parent_contracts.push_back({
                .frame_id                              = contract.frame_id,
                .logical_queue                         = contract.logical_queue,
                .native_queue_id                       = contract.native_queue_id,
                .family_id                             = contract.family_id,
                .source_order                          = contract.source_order,
                .parent_depth                          = contract.parent_depth,
                .child_local_order_deadline            = contract.child_local_order,
                .latest_parent_local_order             = latest_parent_local_order,
                .observed_local_orders_before_deadline = observed_before,
                .timing_topology_trusted               = timing_topology_trusted,
                .observed_ancestor_index               = observed_ancestor_index,
                .last_child_subtree_end_local          = last_child_subtree_end_local,
                .timing_envelope_begin_tick            = envelope_begin_tick,
                .timing_envelope_duration              = envelope_end_offset,
            });
            evidence_begin = evidence_end;
        }

        std::sort(
            missing_parent_contracts.begin(),
            missing_parent_contracts.end(),
            [](const MissingParentContract& _left, const MissingParentContract& _right) {
                if (_left.logical_queue != _right.logical_queue) {
                    return _left.logical_queue < _right.logical_queue;
                }
                if (_left.native_queue_id != _right.native_queue_id) {
                    return _left.native_queue_id < _right.native_queue_id;
                }
                if (_left.family_id != _right.family_id) {
                    return _left.family_id < _right.family_id;
                }
                if (_left.frame_id != _right.frame_id) {
                    return _left.frame_id < _right.frame_id;
                }
                if (_left.source_order != _right.source_order) {
                    return _left.source_order < _right.source_order;
                }
                return _left.child_local_order_deadline < _right.child_local_order_deadline;
            }
        );

        struct SourceLossEvidence {
            std::uint64_t       frame_id{0};
            ProfileLogicalQueue logical_queue{ProfileLogicalQueue::Graphics};
            std::uint32_t       native_queue_id{0};
            std::uint32_t       family_id{0};
            std::uint64_t       source_order{0};
            std::uint64_t       first_scope{0};
            std::uint64_t       scope_count{0};
            std::uint64_t       local_hole_count{0};
            std::uint64_t       required_parent_record_count{0};
        };
        std::vector<SourceLossEvidence> source_loss_evidence;
        source_loss_evidence.reserve(scopes.size());
        std::size_t source_begin = 0;
        while (source_begin < scopes.size()) {
            std::size_t source_end = source_begin + 1;
            while (source_end < scopes.size()) {
                const GpuScopeRecord& first = scopes[source_begin];
                const GpuScopeRecord& next  = scopes[source_end];
                if (first.logical_queue != next.logical_queue ||
                    first.native_queue_id != next.native_queue_id || first.family_id != next.family_id ||
                    first.frame_id != next.frame_id || first.source_order != next.source_order) {
                    break;
                }
                ++source_end;
            }

            for (std::size_t index = source_begin; index < source_end; ++index) {
                if (scopes[index].depth > scopes[index].local_order) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "GpuScope depth cannot be reached by its source local order"
                    );
                    return false;
                }
            }

            std::uint64_t local_span = 0;
            if (AddOverflow(scopes[source_end - 1].local_order, std::uint64_t{1}, local_span)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeIdentityInvalid,
                    "GpuScope source local-order span overflows"
                );
                return false;
            }
            const std::uint64_t observed_count = static_cast<std::uint64_t>(source_end - source_begin);
            source_loss_evidence.push_back({
                .frame_id                     = scopes[source_begin].frame_id,
                .logical_queue                = scopes[source_begin].logical_queue,
                .native_queue_id              = scopes[source_begin].native_queue_id,
                .family_id                    = scopes[source_begin].family_id,
                .source_order                 = scopes[source_begin].source_order,
                .first_scope                  = source_begin,
                .scope_count                  = observed_count,
                .local_hole_count             = local_span - observed_count,
                .required_parent_record_count = 0,
            });
            source_begin = source_end;
        }

        const auto same_contract_source = [](const MissingParentContract& _contract,
                                             const SourceLossEvidence&    _source) {
            return _contract.logical_queue == _source.logical_queue &&
                   _contract.native_queue_id == _source.native_queue_id &&
                   _contract.family_id == _source.family_id && _contract.frame_id == _source.frame_id &&
                   _contract.source_order == _source.source_order;
        };
        const auto source_less_than_contract = [](const SourceLossEvidence&    _source,
                                                  const MissingParentContract& _contract) {
            return std::tie(
                       _source.logical_queue,
                       _source.native_queue_id,
                       _source.family_id,
                       _source.frame_id,
                       _source.source_order
                   ) <
                   std::tie(
                       _contract.logical_queue,
                       _contract.native_queue_id,
                       _contract.family_id,
                       _contract.frame_id,
                       _contract.source_order
                   );
        };

        std::size_t contract_begin = 0;
        while (contract_begin < missing_parent_contracts.size()) {
            std::size_t contract_end = contract_begin + 1;
            while (contract_end < missing_parent_contracts.size()) {
                const MissingParentContract& first = missing_parent_contracts[contract_begin];
                const MissingParentContract& next  = missing_parent_contracts[contract_end];
                if (first.logical_queue != next.logical_queue ||
                    first.native_queue_id != next.native_queue_id || first.family_id != next.family_id ||
                    first.frame_id != next.frame_id || first.source_order != next.source_order) {
                    break;
                }
                ++contract_end;
            }

            SourceLossEvidence* source    = nullptr;
            const auto          source_it = std::lower_bound(
                source_loss_evidence.begin(),
                source_loss_evidence.end(),
                missing_parent_contracts[contract_begin],
                source_less_than_contract
            );
            if (source_it != source_loss_evidence.end() &&
                same_contract_source(missing_parent_contracts[contract_begin], *source_it)) {
                source = &*source_it;
            }
            if (source == nullptr) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeParentInvalid,
                    "missing GpuScope parent has no recording source"
                );
                return false;
            }

            std::vector<std::uint32_t> depth_coordinates;
            depth_coordinates.reserve(
                static_cast<std::size_t>(source->scope_count) + contract_end - contract_begin
            );
            const std::size_t source_end_index =
                static_cast<std::size_t>(source->first_scope + source->scope_count);
            std::uint64_t last_observed_root_local_order = 0;
            bool          has_observed_root              = false;
            for (std::size_t index = static_cast<std::size_t>(source->first_scope); index < source_end_index;
                 ++index) {
                depth_coordinates.push_back(scopes[index].depth);
                if (scopes[index].parent_scope_id == 0) {
                    last_observed_root_local_order = scopes[index].local_order;
                    has_observed_root              = true;
                }
            }
            for (std::size_t index = contract_begin; index < contract_end; ++index) {
                depth_coordinates.push_back(missing_parent_contracts[index].parent_depth);
            }
            std::sort(depth_coordinates.begin(), depth_coordinates.end());
            depth_coordinates.erase(
                std::unique(depth_coordinates.begin(), depth_coordinates.end()), depth_coordinates.end()
            );

            std::vector<std::uint64_t> covered_depth_tree(depth_coordinates.size() + 1, 0);
            std::vector<std::uint64_t> prefix_covered_depth_tree(depth_coordinates.size() + 1, 0);
            std::vector<std::uint8_t>  depth_covered(depth_coordinates.size(), 0);
            std::vector<std::uint8_t>  prefix_depth_covered(depth_coordinates.size(), 0);
            const auto                 cover_depth = [&](std::uint32_t               _depth,
                                         std::vector<std::uint64_t>& _tree,
                                         std::vector<std::uint8_t>&  _covered) {
                const std::size_t coordinate = static_cast<std::size_t>(
                    std::lower_bound(depth_coordinates.begin(), depth_coordinates.end(), _depth) -
                    depth_coordinates.begin()
                );
                if (_covered[coordinate] != 0) {
                    return;
                }
                _covered[coordinate] = 1;
                for (std::size_t tree_index = coordinate + 1; tree_index < _tree.size();
                     tree_index += tree_index & (~tree_index + 1)) {
                    ++_tree[tree_index];
                }
            };
            const auto covered_depth_count_before = [&](std::uint32_t                     _depth,
                                                        const std::vector<std::uint64_t>& _tree) {
                std::size_t tree_index = static_cast<std::size_t>(
                    std::lower_bound(depth_coordinates.begin(), depth_coordinates.end(), _depth) -
                    depth_coordinates.begin()
                );
                std::uint64_t covered_count = 0;
                while (tree_index != 0) {
                    covered_count += _tree[tree_index];
                    tree_index &= tree_index - 1;
                }
                return covered_count;
            };

            for (std::size_t index = contract_begin; index < contract_end; ++index) {
                cover_depth(missing_parent_contracts[index].parent_depth, covered_depth_tree, depth_covered);
            }
            const bool timing_topology_trusted =
                missing_parent_contracts[contract_begin].timing_topology_trusted;
            if (timing_topology_trusted) {
                struct MissingTaskKey {
                    std::uint64_t anchor_index{kInvalidSessionIndex};
                    std::uint32_t depth{0};
                    std::uint64_t first_allowed_slot{0};

                    [[nodiscard]] bool operator<(const MissingTaskKey& _right) const noexcept {
                        return std::tie(anchor_index, depth, first_allowed_slot) <
                               std::tie(_right.anchor_index, _right.depth, _right.first_allowed_slot);
                    }
                };
                struct MissingTaskState {
                    std::uint64_t canonical_task_index{kInvalidSessionIndex};
                    bool          canonical_is_direct{false};
                };
                struct MissingTask {
                    std::uint64_t first_allowed_slot{0};
                    std::uint64_t deadline_slot{0};
                    std::uint64_t assigned_slot{kInvalidSessionIndex};
                    std::uint32_t depth{0};
                    bool          has_timing_envelope{false};
                    std::uint64_t timing_envelope_begin_tick{0};
                    std::uint64_t timing_envelope_duration{0};
                    std::uint64_t timing_envelope_member_count{0};
                    std::uint64_t maximum_member_duration{0};
                    std::uint64_t first_member_duration{0};
                    std::uint64_t first_member_subtree_end_local{0};
                    std::uint64_t timing_subtree_end_local{0};
                };
                struct RootPartitionCandidate {
                    std::uint64_t root_task_index{kInvalidSessionIndex};
                    std::uint64_t previous_subtree_end_local{0};
                    std::uint64_t child_local_order_deadline{0};
                    std::uint64_t member_begin_tick{0};
                    std::uint64_t member_duration{0};
                    std::uint64_t member_subtree_end_local{0};
                    std::size_t   chain_begin{0};
                    std::size_t   chain_end{0};
                };

                std::vector<std::uint64_t> latest_barrier_tree(depth_coordinates.size() + 1, 0);
                const auto                 observe_barrier = [&](std::size_t _scope_index) {
                    const GpuScopeRecord& scope      = scopes[_scope_index];
                    const std::size_t     coordinate = static_cast<std::size_t>(
                        std::lower_bound(depth_coordinates.begin(), depth_coordinates.end(), scope.depth) -
                        depth_coordinates.begin()
                    );
                    const std::uint64_t local_order_after = timing_subtree_end_local[_scope_index] + 1;
                    for (std::size_t tree_index = coordinate + 1; tree_index < latest_barrier_tree.size();
                         tree_index += tree_index & (~tree_index + 1)) {
                        latest_barrier_tree[tree_index] =
                            std::max(latest_barrier_tree[tree_index], local_order_after);
                    }
                };
                const auto latest_barrier_after = [&](std::uint32_t _depth) {
                    std::size_t tree_index = static_cast<std::size_t>(
                        std::upper_bound(depth_coordinates.begin(), depth_coordinates.end(), _depth) -
                        depth_coordinates.begin()
                    );
                    std::uint64_t latest = 0;
                    while (tree_index != 0) {
                        latest = std::max(latest, latest_barrier_tree[tree_index]);
                        tree_index &= tree_index - 1;
                    }
                    return latest;
                };

                std::map<MissingTaskKey, MissingTaskState> task_states;
                std::vector<MissingTask>                   tasks;
                std::vector<std::uint64_t>                 chain_task_indices;
                std::vector<std::uint8_t>                  chain_task_shared_with_prior;
                std::vector<std::uint64_t>                 chain_ends;
                std::vector<RootPartitionCandidate>        root_partition_candidates;
                std::map<std::uint32_t, std::uint64_t>     previous_missing_direct_end;
                std::map<std::uint32_t, std::uint64_t>     virtual_epoch_floor;
                std::vector<std::size_t>                   completion_order;
                completion_order.reserve(static_cast<std::size_t>(source->scope_count));
                for (std::size_t index = static_cast<std::size_t>(source->first_scope);
                     index < source_end_index;
                     ++index) {
                    completion_order.push_back(index);
                }
                std::sort(
                    completion_order.begin(),
                    completion_order.end(),
                    [&](std::size_t _left, std::size_t _right) {
                        return std::tie(timing_subtree_end_local[_left], scopes[_left].local_order) <
                               std::tie(timing_subtree_end_local[_right], scopes[_right].local_order);
                    }
                );
                std::size_t completed_observed_index = 0;
                for (std::size_t index = contract_begin; index < contract_end; ++index) {
                    const MissingParentContract& contract = missing_parent_contracts[index];
                    while (completed_observed_index < completion_order.size() &&
                           timing_subtree_end_local[completion_order[completed_observed_index]] <
                               contract.child_local_order_deadline) {
                        observe_barrier(completion_order[completed_observed_index]);
                        ++completed_observed_index;
                    }

                    const std::uint64_t anchor_index = contract.observed_ancestor_index;
                    const std::uint32_t base_depth =
                        anchor_index == kInvalidSessionIndex ? 0 : scopes[anchor_index].depth + 1;
                    const std::uint64_t chain_item_count =
                        static_cast<std::uint64_t>(contract.parent_depth - base_depth) + 1;
                    if (!charge_topology_work(chain_item_count)) {
                        return false;
                    }

                    const std::uint64_t anchor_first_allowed =
                        anchor_index == kInvalidSessionIndex ? 0 : scopes[anchor_index].local_order + 1;
                    const auto previous_direct = previous_missing_direct_end.find(contract.parent_depth);
                    if (previous_direct != previous_missing_direct_end.end()) {
                        std::uint64_t sibling_after = 0;
                        if (AddOverflow(previous_direct->second, std::uint64_t{1}, sibling_after)) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeIdentityInvalid,
                                "missing GPU parent subtree overflows local-order space"
                            );
                            return false;
                        }
                        if (sibling_after >= contract.child_local_order_deadline) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeParentInvalid,
                                "distinct missing GPU parents at one depth have interleaved subtrees"
                            );
                            return false;
                        }
                        virtual_epoch_floor[contract.parent_depth] = sibling_after;
                    }
                    const std::size_t current_chain_begin = chain_task_indices.size();
                    for (std::uint32_t depth = base_depth;; ++depth) {
                        std::uint64_t first_allowed_slot =
                            std::max(anchor_first_allowed, latest_barrier_after(depth));
                        const auto epoch = virtual_epoch_floor.find(depth);
                        if (epoch != virtual_epoch_floor.end()) {
                            first_allowed_slot = std::max(first_allowed_slot, epoch->second);
                        }
                        const bool direct = depth == contract.parent_depth;
                        if (first_allowed_slot >= contract.child_local_order_deadline) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeParentInvalid,
                                "missing GPU ancestry has no slot after its timing barrier"
                            );
                            return false;
                        }

                        const MissingTaskKey key{
                            .anchor_index       = anchor_index,
                            .depth              = depth,
                            .first_allowed_slot = first_allowed_slot,
                        };
                        auto [state_it, state_inserted]          = task_states.try_emplace(key);
                        MissingTaskState& state                  = state_it->second;
                        std::uint64_t     task_index             = state.canonical_task_index;
                        bool              task_shared_with_prior = false;
                        if (state_inserted) {
                            task_index = static_cast<std::uint64_t>(tasks.size());
                            tasks.push_back({
                                .first_allowed_slot = first_allowed_slot,
                                .deadline_slot      = contract.child_local_order_deadline,
                                .depth              = depth,
                            });
                            state.canonical_task_index = task_index;
                            state.canonical_is_direct  = direct;
                        } else if (direct && state.canonical_is_direct) {
                            task_index = static_cast<std::uint64_t>(tasks.size());
                            tasks.push_back({
                                .first_allowed_slot = first_allowed_slot,
                                .deadline_slot      = contract.child_local_order_deadline,
                                .depth              = depth,
                            });
                        } else {
                            task_shared_with_prior = true;
                            MissingTask& task      = tasks[task_index];
                            task.deadline_slot =
                                std::min(task.deadline_slot, contract.child_local_order_deadline);
                            if (direct) {
                                state.canonical_is_direct = true;
                            }
                        }
                        chain_task_indices.push_back(task_index);
                        chain_task_shared_with_prior.push_back(
                            static_cast<std::uint8_t>(task_shared_with_prior)
                        );
                        if (depth == contract.parent_depth) {
                            break;
                        }
                    }
                    if (base_depth == 0) {
                        const std::uint64_t root_task_index = chain_task_indices[current_chain_begin];
                        MissingTask&        root_task       = tasks[root_task_index];
                        if (!root_task.has_timing_envelope) {
                            root_task.has_timing_envelope            = true;
                            root_task.timing_envelope_begin_tick     = contract.timing_envelope_begin_tick;
                            root_task.timing_envelope_duration       = contract.timing_envelope_duration;
                            root_task.timing_envelope_member_count   = 1;
                            root_task.maximum_member_duration        = contract.timing_envelope_duration;
                            root_task.first_member_duration          = contract.timing_envelope_duration;
                            root_task.first_member_subtree_end_local = contract.last_child_subtree_end_local;
                            root_task.timing_subtree_end_local       = contract.last_child_subtree_end_local;
                        } else {
                            const std::uint32_t valid_bits =
                                scopes[static_cast<std::size_t>(source->first_scope)].valid_bits;
                            const std::uint64_t envelope_begin_offset =
                                (contract.timing_envelope_begin_tick - root_task.timing_envelope_begin_tick) &
                                TimestampMask(valid_bits);
                            const std::uint64_t timestamp_mask = TimestampMask(valid_bits);
                            const bool          envelope_invalid =
                                envelope_begin_offset < root_task.timing_envelope_duration ||
                                contract.timing_envelope_duration > timestamp_mask - envelope_begin_offset;
                            const std::uint64_t merged_envelope_duration =
                                envelope_invalid ? 0 :
                                                   envelope_begin_offset + contract.timing_envelope_duration;
                            const std::uint64_t maximum_root_step =
                                valid_bits == 64 ? (std::uint64_t{1} << 63) - 1 :
                                                   (std::uint64_t{1} << (valid_bits - 1)) - 1;
                            const bool has_later_observed_root =
                                has_observed_root &&
                                last_observed_root_local_order > contract.last_child_subtree_end_local;
                            if (envelope_invalid ||
                                (has_later_observed_root && merged_envelope_duration > maximum_root_step)) {
                                const std::size_t chain_count =
                                    chain_task_indices.size() - current_chain_begin;
                                if (!charge_topology_work(static_cast<std::uint64_t>(chain_count))) {
                                    return false;
                                }
                                std::uint64_t split_first_allowed = 0;
                                if (AddOverflow(
                                        root_task.timing_subtree_end_local,
                                        std::uint64_t{1},
                                        split_first_allowed
                                    ) ||
                                    split_first_allowed >= contract.child_local_order_deadline) {
                                    Fail(
                                        SessionLoadStatus::ProtocolViolation,
                                        SessionErrorCode::GpuScopeParentInvalid,
                                        "a virtual GPU root cannot split before the next child subtree"
                                    );
                                    return false;
                                }
                                for (std::size_t chain_index = current_chain_begin;
                                     chain_index < chain_task_indices.size();
                                     ++chain_index) {
                                    MissingTask clone = tasks[chain_task_indices[chain_index]];
                                    clone.first_allowed_slot =
                                        std::max(clone.first_allowed_slot, split_first_allowed);
                                    clone.deadline_slot                = contract.child_local_order_deadline;
                                    clone.assigned_slot                = kInvalidSessionIndex;
                                    clone.has_timing_envelope          = false;
                                    clone.timing_envelope_begin_tick   = 0;
                                    clone.timing_envelope_duration     = 0;
                                    clone.timing_envelope_member_count = 0;
                                    clone.maximum_member_duration      = 0;
                                    clone.first_member_duration        = 0;
                                    clone.first_member_subtree_end_local = 0;
                                    clone.timing_subtree_end_local       = 0;
                                    const std::uint64_t clone_index =
                                        static_cast<std::uint64_t>(tasks.size());
                                    tasks.push_back(clone);
                                    chain_task_indices[chain_index]           = clone_index;
                                    chain_task_shared_with_prior[chain_index] = 0;
                                    const MissingTaskKey clone_key{
                                        .anchor_index       = anchor_index,
                                        .depth              = clone.depth,
                                        .first_allowed_slot = clone.first_allowed_slot,
                                    };
                                    MissingTaskState& clone_state    = task_states[clone_key];
                                    clone_state.canonical_task_index = clone_index;
                                    clone_state.canonical_is_direct  = clone.depth == contract.parent_depth;
                                }
                                MissingTask& split_root = tasks[chain_task_indices[current_chain_begin]];
                                split_root.has_timing_envelope          = true;
                                split_root.timing_envelope_begin_tick   = contract.timing_envelope_begin_tick;
                                split_root.timing_envelope_duration     = contract.timing_envelope_duration;
                                split_root.timing_envelope_member_count = 1;
                                split_root.maximum_member_duration      = contract.timing_envelope_duration;
                                split_root.first_member_duration        = contract.timing_envelope_duration;
                                split_root.first_member_subtree_end_local =
                                    contract.last_child_subtree_end_local;
                                split_root.timing_subtree_end_local = contract.last_child_subtree_end_local;
                            } else {
                                root_partition_candidates.push_back({
                                    .root_task_index            = root_task_index,
                                    .previous_subtree_end_local = root_task.timing_subtree_end_local,
                                    .child_local_order_deadline = contract.child_local_order_deadline,
                                    .member_begin_tick          = contract.timing_envelope_begin_tick,
                                    .member_duration            = contract.timing_envelope_duration,
                                    .member_subtree_end_local   = contract.last_child_subtree_end_local,
                                    .chain_begin                = current_chain_begin,
                                    .chain_end                  = chain_task_indices.size(),
                                });
                                root_task.timing_envelope_duration = merged_envelope_duration;
                                ++root_task.timing_envelope_member_count;
                                root_task.maximum_member_duration = std::max(
                                    root_task.maximum_member_duration, contract.timing_envelope_duration
                                );
                                root_task.timing_subtree_end_local = std::max(
                                    root_task.timing_subtree_end_local, contract.last_child_subtree_end_local
                                );
                            }
                        }
                    }
                    chain_ends.push_back(static_cast<std::uint64_t>(chain_task_indices.size()));
                    previous_missing_direct_end[contract.parent_depth] =
                        contract.last_child_subtree_end_local;
                }

                std::vector<std::uint8_t> task_active(tasks.size(), 0);
                std::uint64_t             active_task_count = 0;
                for (const std::uint64_t task_index : chain_task_indices) {
                    if (task_active[static_cast<std::size_t>(task_index)] == 0) {
                        task_active[static_cast<std::size_t>(task_index)] = 1;
                        ++active_task_count;
                    }
                }
                source->required_parent_record_count = active_task_count;
                if (source->required_parent_record_count > source->local_hole_count) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "missing GPU ancestry exceeds its source local-order holes"
                    );
                    return false;
                }
                const bool has_spare_local_holes =
                    source->required_parent_record_count < source->local_hole_count;
                std::vector<std::uint64_t> task_order;
                task_order.reserve(static_cast<std::size_t>(active_task_count));
                for (std::size_t index = 0; index < tasks.size(); ++index) {
                    if (task_active[index] != 0) {
                        task_order.push_back(static_cast<std::uint64_t>(index));
                    }
                }
                std::sort(
                    task_order.begin(),
                    task_order.end(),
                    [&](std::uint64_t _left, std::uint64_t _right) {
                        const MissingTask& left  = tasks[_left];
                        const MissingTask& right = tasks[_right];
                        return std::tie(left.deadline_slot, left.first_allowed_slot, left.depth, _left) >
                               std::tie(right.deadline_slot, right.first_allowed_slot, right.depth, _right);
                    }
                );
                const auto earlier_release = [&](std::uint64_t _left, std::uint64_t _right) {
                    const MissingTask& left  = tasks[_left];
                    const MissingTask& right = tasks[_right];
                    return std::tie(left.first_allowed_slot, left.depth, _left) <
                           std::tie(right.first_allowed_slot, right.depth, _right);
                };
                std::priority_queue<std::uint64_t, std::vector<std::uint64_t>, decltype(earlier_release)>
                              ready_tasks(earlier_release);
                std::size_t   ordered_task_index  = 0;
                std::size_t   observed_slot_index = source_end_index;
                bool          has_slot            = !task_order.empty();
                std::uint64_t slot = task_order.empty() ? 0 : tasks[task_order.front()].deadline_slot - 1;
                const auto    move_to_previous_slot = [&]() {
                    if (slot == 0) {
                        has_slot = false;
                    } else {
                        --slot;
                    }
                };
                while (ordered_task_index < task_order.size() || !ready_tasks.empty()) {
                    if (!has_slot) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "missing GPU ancestry cannot fit its sparse local-order holes"
                        );
                        return false;
                    }
                    while (ordered_task_index < task_order.size() &&
                           tasks[task_order[ordered_task_index]].deadline_slot > slot) {
                        ready_tasks.push(task_order[ordered_task_index]);
                        ++ordered_task_index;
                    }
                    if (ready_tasks.empty()) {
                        slot = tasks[task_order[ordered_task_index]].deadline_slot - 1;
                        continue;
                    }
                    while (observed_slot_index > static_cast<std::size_t>(source->first_scope) &&
                           scopes[observed_slot_index - 1].local_order > slot) {
                        --observed_slot_index;
                    }
                    if (observed_slot_index > static_cast<std::size_t>(source->first_scope) &&
                        scopes[observed_slot_index - 1].local_order == slot) {
                        --observed_slot_index;
                        move_to_previous_slot();
                        continue;
                    }
                    const std::uint64_t task_index = ready_tasks.top();
                    if (tasks[task_index].first_allowed_slot > slot) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "missing GPU ancestry cannot fit its sparse local-order holes"
                        );
                        return false;
                    }
                    ready_tasks.pop();
                    tasks[task_index].assigned_slot = slot;
                    move_to_previous_slot();
                }

                std::size_t chain_begin = 0;
                for (const std::uint64_t chain_end_value : chain_ends) {
                    const std::size_t chain_end = static_cast<std::size_t>(chain_end_value);
                    for (std::size_t index = chain_begin + 1; index < chain_end; ++index) {
                        if (tasks[chain_task_indices[index - 1]].assigned_slot >=
                            tasks[chain_task_indices[index]].assigned_slot) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeParentInvalid,
                                "missing GPU ancestry cannot preserve parent-before-child order"
                            );
                            return false;
                        }
                    }
                    chain_begin = chain_end;
                }

                std::vector<std::uint64_t> occupied_local_orders;
                occupied_local_orders.reserve(
                    static_cast<std::size_t>(source->scope_count) +
                    static_cast<std::size_t>(active_task_count)
                );
                for (std::size_t index = static_cast<std::size_t>(source->first_scope);
                     index < source_end_index;
                     ++index) {
                    occupied_local_orders.push_back(scopes[index].local_order);
                }
                for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
                    if (task_active[task_index] != 0) {
                        occupied_local_orders.push_back(tasks[task_index].assigned_slot);
                    }
                }
                std::sort(occupied_local_orders.begin(), occupied_local_orders.end());

                std::vector<std::uint64_t> candidate_split_root_slot(
                    root_partition_candidates.size(), kInvalidSessionIndex
                );
                std::vector<std::size_t>   candidate_reserved_slot_begin(root_partition_candidates.size(), 0);
                std::vector<std::size_t>   candidate_reserved_slot_end(root_partition_candidates.size(), 0);
                std::vector<std::uint64_t> candidate_reserved_slot_storage;
                std::vector<std::uint8_t>  task_has_concrete_split(tasks.size(), 0);
                std::set<std::uint64_t>    reserved_split_slots;
                std::vector<std::size_t>   candidate_order;
                candidate_order.reserve(root_partition_candidates.size());
                for (std::size_t index = 0; index < root_partition_candidates.size(); ++index) {
                    candidate_order.push_back(index);
                }
                std::sort(
                    candidate_order.begin(),
                    candidate_order.end(),
                    [&](std::size_t _left, std::size_t _right) {
                        return std::tie(root_partition_candidates[_left].root_task_index, _left) <
                               std::tie(root_partition_candidates[_right].root_task_index, _right);
                    }
                );
                if (has_spare_local_holes) {

                    std::size_t group_begin = 0;
                    while (group_begin < candidate_order.size()) {
                        std::size_t       group_end       = group_begin + 1;
                        const std::size_t root_task_index = static_cast<std::size_t>(
                            root_partition_candidates[candidate_order[group_begin]].root_task_index
                        );
                        while (group_end < candidate_order.size() &&
                               root_partition_candidates[candidate_order[group_end]].root_task_index ==
                                   root_partition_candidates[candidate_order[group_begin]].root_task_index) {
                            ++group_end;
                        }
                        if (task_active[root_task_index] == 0 ||
                            tasks[root_task_index].timing_envelope_member_count !=
                                static_cast<std::uint64_t>(group_end - group_begin + 1)) {
                            group_begin = group_end;
                            continue;
                        }

                        for (std::size_t order_index = group_begin; order_index < group_end; ++order_index) {
                            const std::size_t             candidate_index = candidate_order[order_index];
                            const RootPartitionCandidate& candidate =
                                root_partition_candidates[candidate_index];
                            const std::uint64_t chain_item_count =
                                static_cast<std::uint64_t>(candidate.chain_end - candidate.chain_begin);
                            if (!charge_topology_work(chain_item_count)) {
                                return false;
                            }

                            std::uint64_t required_split_records = 0;
                            std::uint64_t first_unshared_slot    = candidate.child_local_order_deadline;
                            bool          reached_unshared_task  = false;
                            bool          candidate_feasible     = true;
                            for (std::size_t chain_index = candidate.chain_begin;
                                 chain_index < candidate.chain_end;
                                 ++chain_index) {
                                const std::size_t task_index =
                                    static_cast<std::size_t>(chain_task_indices[chain_index]);
                                if (chain_task_shared_with_prior[chain_index] != 0) {
                                    if (reached_unshared_task) {
                                        candidate_feasible = false;
                                        break;
                                    }
                                    ++required_split_records;
                                } else {
                                    reached_unshared_task = true;
                                    first_unshared_slot =
                                        std::min(first_unshared_slot, tasks[task_index].assigned_slot);
                                }
                            }
                            if (!candidate_feasible || required_split_records == 0 ||
                                candidate.previous_subtree_end_local >= first_unshared_slot) {
                                continue;
                            }

                            std::vector<std::uint64_t> candidate_reserved_slots;
                            std::uint64_t              cursor    = first_unshared_slot;
                            std::uint64_t              root_slot = kInvalidSessionIndex;
                            for (std::uint64_t record = 0; record < required_split_records; ++record) {
                                bool found = false;
                                while (cursor != 0) {
                                    --cursor;
                                    if (cursor <= candidate.previous_subtree_end_local) {
                                        break;
                                    }
                                    if (!charge_topology_work(1)) {
                                        return false;
                                    }
                                    if (!std::binary_search(
                                            occupied_local_orders.begin(), occupied_local_orders.end(), cursor
                                        ) &&
                                        !reserved_split_slots.contains(cursor)) {
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    candidate_feasible = false;
                                    break;
                                }
                                reserved_split_slots.insert(cursor);
                                candidate_reserved_slots.push_back(cursor);
                                root_slot = cursor;
                            }
                            if (candidate_feasible) {
                                candidate_reserved_slot_begin[candidate_index] =
                                    candidate_reserved_slot_storage.size();
                                candidate_reserved_slot_storage.insert(
                                    candidate_reserved_slot_storage.end(),
                                    candidate_reserved_slots.begin(),
                                    candidate_reserved_slots.end()
                                );
                                candidate_reserved_slot_end[candidate_index] =
                                    candidate_reserved_slot_storage.size();
                                candidate_split_root_slot[candidate_index] = root_slot;
                                task_has_concrete_split[root_task_index]   = 1;
                            } else {
                                for (const std::uint64_t slot : candidate_reserved_slots) {
                                    reserved_split_slots.erase(slot);
                                }
                            }
                        }
                        group_begin = group_end;
                    }
                }

                struct RootComponent {
                    std::uint64_t local_order{0};
                    std::uint64_t subtree_end_local{0};
                    bool          flexible{false};
                    std::uint64_t begin_tick{0};
                    std::uint64_t duration{0};
                    std::uint64_t atomic_duration{0};
                };
                std::vector<RootComponent> root_components;
                std::vector<RootComponent> split_root_components;
                std::set<std::uint64_t>    used_split_slots;
                root_components.reserve(static_cast<std::size_t>(source->scope_count) + tasks.size());
                split_root_components.reserve(
                    static_cast<std::size_t>(source->scope_count) + tasks.size() +
                    root_partition_candidates.size()
                );
                for (std::size_t index = static_cast<std::size_t>(source->first_scope);
                     index < source_end_index;
                     ++index) {
                    const GpuScopeRecord& scope = scopes[index];
                    if (scope.parent_scope_id == 0) {
                        const RootComponent component{
                            .local_order       = scope.local_order,
                            .subtree_end_local = timing_subtree_end_local[index],
                            .flexible          = false,
                            .begin_tick        = scope.begin_tick,
                            .duration = TimestampDelta(scope.begin_tick, scope.end_tick, scope.valid_bits),
                            .atomic_duration =
                                TimestampDelta(scope.begin_tick, scope.end_tick, scope.valid_bits),
                        };
                        root_components.push_back(component);
                        split_root_components.push_back(component);
                    }
                }
                std::size_t         candidate_order_index = 0;
                bool                has_split_plan        = false;
                const std::uint32_t source_valid_bits =
                    scopes[static_cast<std::size_t>(source->first_scope)].valid_bits;
                const std::uint64_t source_timestamp_mask    = TimestampMask(source_valid_bits);
                const std::uint64_t source_half_range        = source_valid_bits == 64 ?
                                                                   (std::uint64_t{1} << 63) :
                                                                   (std::uint64_t{1} << (source_valid_bits - 1));
                const std::uint64_t source_maximum_root_step = source_half_range - 1;
                for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
                    while (candidate_order_index < candidate_order.size() &&
                           root_partition_candidates[candidate_order[candidate_order_index]].root_task_index <
                               task_index) {
                        ++candidate_order_index;
                    }
                    const std::size_t task_candidate_begin = candidate_order_index;
                    while (
                        candidate_order_index < candidate_order.size() &&
                        root_partition_candidates[candidate_order[candidate_order_index]].root_task_index ==
                            task_index
                    ) {
                        ++candidate_order_index;
                    }
                    const std::size_t task_candidate_end = candidate_order_index;
                    if (task_active[task_index] == 0) {
                        continue;
                    }
                    const MissingTask& task = tasks[task_index];
                    if (task.depth != 0) {
                        continue;
                    }
                    if (!task.has_timing_envelope || task.assigned_slot == kInvalidSessionIndex) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "virtual GPU root has no assigned timing envelope"
                        );
                        return false;
                    }
                    const RootComponent merged_component{
                        .local_order       = task.assigned_slot,
                        .subtree_end_local = task.timing_subtree_end_local,
                        .flexible          = true,
                        .begin_tick        = task.timing_envelope_begin_tick,
                        .duration          = task.timing_envelope_duration,
                        .atomic_duration   = task.maximum_member_duration,
                    };
                    root_components.push_back(merged_component);
                    if (task_has_concrete_split[task_index] == 0) {
                        split_root_components.push_back(merged_component);
                        continue;
                    }

                    RootComponent split_component{
                        .local_order       = task.assigned_slot,
                        .subtree_end_local = task.first_member_subtree_end_local,
                        .flexible          = true,
                        .begin_tick        = task.timing_envelope_begin_tick,
                        .duration          = task.first_member_duration,
                        .atomic_duration   = task.first_member_duration,
                    };
                    for (std::size_t order_index = task_candidate_begin; order_index < task_candidate_end;
                         ++order_index) {
                        const std::size_t             candidate_index = candidate_order[order_index];
                        const RootPartitionCandidate& candidate = root_partition_candidates[candidate_index];
                        const std::uint64_t split_root_slot     = candidate_split_root_slot[candidate_index];
                        if (split_root_slot != kInvalidSessionIndex &&
                            split_component.duration <= source_maximum_root_step) {
                            has_split_plan = true;
                            for (std::size_t slot_index = candidate_reserved_slot_begin[candidate_index];
                                 slot_index < candidate_reserved_slot_end[candidate_index];
                                 ++slot_index) {
                                used_split_slots.insert(candidate_reserved_slot_storage[slot_index]);
                            }
                            split_root_components.push_back(split_component);
                            split_component = {
                                .local_order       = split_root_slot,
                                .subtree_end_local = candidate.member_subtree_end_local,
                                .flexible          = true,
                                .begin_tick        = candidate.member_begin_tick,
                                .duration          = candidate.member_duration,
                                .atomic_duration   = candidate.member_duration,
                            };
                            continue;
                        }

                        const std::uint64_t member_begin_offset =
                            (candidate.member_begin_tick - split_component.begin_tick) &
                            source_timestamp_mask;
                        if (member_begin_offset < split_component.duration ||
                            candidate.member_duration > source_timestamp_mask - member_begin_offset) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeTimingInvalid,
                                "concrete GPU root partition has a non-serial member envelope"
                            );
                            return false;
                        }
                        split_component.duration = member_begin_offset + candidate.member_duration;
                        split_component.atomic_duration =
                            std::max(split_component.atomic_duration, candidate.member_duration);
                        split_component.subtree_end_local =
                            std::max(split_component.subtree_end_local, candidate.member_subtree_end_local);
                    }
                    split_root_components.push_back(split_component);
                }
                const auto sort_root_components = [](std::vector<RootComponent>& _components) {
                    std::sort(
                        _components.begin(),
                        _components.end(),
                        [](const RootComponent& _left, const RootComponent& _right) {
                            return std::tie(_left.local_order, _left.flexible) <
                                   std::tie(_right.local_order, _right.flexible);
                        }
                    );
                };
                sort_root_components(root_components);
                sort_root_components(split_root_components);
                const auto duplicate_root_slot = [](const std::vector<RootComponent>& _components) {
                    return std::adjacent_find(
                               _components.begin(),
                               _components.end(),
                               [](const RootComponent& _left, const RootComponent& _right) {
                                   return _left.local_order == _right.local_order;
                               }
                           ) != _components.end();
                };
                if (duplicate_root_slot(root_components) ||
                    (has_split_plan && duplicate_root_slot(split_root_components))) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "observed and virtual GPU roots occupy one local-order slot"
                    );
                    return false;
                }
                if (!root_components.empty()) {
                    const std::uint32_t valid_bits =
                        scopes[static_cast<std::size_t>(source->first_scope)].valid_bits;
                    const std::uint64_t timestamp_mask = TimestampMask(valid_bits);
                    const std::uint64_t half_range =
                        valid_bits == 64 ? (std::uint64_t{1} << 63) : (std::uint64_t{1} << (valid_bits - 1));
                    const std::uint64_t maximum_root_step  = half_range - 1;
                    const auto          fail_root_sequence = [&]() {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "observed and virtual GPU roots have no legal serial timestamp order"
                        );
                        return false;
                    };
                    bool       topology_limit_failed  = false;
                    const auto validate_root_sequence = [&](const std::vector<RootComponent>& _components,
                                                            const std::vector<std::uint64_t>&
                                                                _occupied_local_orders) {
                        if (_components.empty()) {
                            return true;
                        }
                        for (std::size_t index = 0; index < _components.size(); ++index) {
                            const RootComponent& component = _components[index];
                            if (index + 1 < _components.size() &&
                                component.atomic_duration > maximum_root_step) {
                                return false;
                            }
                            if (index != 0 &&
                                _components[index - 1].subtree_end_local >= component.local_order) {
                                return false;
                            }
                        }

                        struct RootSerialState {
                            SerialTick begin{};
                            SerialTick end{};
                        };
                        const RootComponent& first_component = _components.front();
                        RootSerialState      initial_state{
                                 .begin =
                                     {
                                         .epoch = 1,
                                         .tick  = first_component.begin_tick & timestamp_mask,
                                },
                        };
                        if (first_component.flexible) {
                            const std::uint64_t maximum_duration =
                                _components.size() == 1 ? timestamp_mask : maximum_root_step;
                            if (first_component.duration > maximum_duration || !AddSerialTickDelta(
                                                                                   initial_state.begin,
                                                                                   first_component.duration,
                                                                                   valid_bits,
                                                                                   initial_state.end
                                                                               )) {
                                return false;
                            }
                        } else if (!AddSerialTickDelta(
                                       initial_state.begin,
                                       first_component.duration,
                                       valid_bits,
                                       initial_state.end
                                   )) {
                            return false;
                        }
                        std::vector<RootSerialState> frontier{initial_state};

                        for (std::size_t index = 1; index < _components.size(); ++index) {
                            const RootComponent& previous_component = _components[index - 1];
                            const RootComponent& component          = _components[index];
                            const auto           occupied_begin     = std::upper_bound(
                                _occupied_local_orders.begin(),
                                _occupied_local_orders.end(),
                                previous_component.subtree_end_local
                            );
                            const auto occupied_end = std::lower_bound(
                                occupied_begin, _occupied_local_orders.end(), component.local_order
                            );
                            const std::uint64_t span =
                                component.local_order - previous_component.subtree_end_local - 1;
                            const std::uint64_t occupied_count =
                                static_cast<std::uint64_t>(occupied_end - occupied_begin);
                            if (occupied_count > span) {
                                return false;
                            }
                            const std::uint64_t free_root_count = span - occupied_count;
                            const bool          final_component = index + 1 == _components.size();
                            const std::uint64_t maximum_duration =
                                final_component ? timestamp_mask : maximum_root_step;
                            if (component.flexible && component.duration > maximum_duration) {
                                return false;
                            }

                            std::vector<RootSerialState> next_frontier;
                            for (const RootSerialState& state : frontier) {
                                if (!charge_topology_work(free_root_count)) {
                                    topology_limit_failed = true;
                                    return false;
                                }
                                SerialTick window_upper{};
                                if (!AddSerialTickDelta(
                                        state.begin, maximum_root_step, valid_bits, window_upper
                                    ) ||
                                    !SerialTickLessEqual(state.end, window_upper)) {
                                    continue;
                                }
                                bool window_overflow = false;
                                for (std::uint64_t step = 0; step < free_root_count; ++step) {
                                    SerialTick next_upper{};
                                    if (!AddSerialTickDelta(
                                            window_upper, maximum_root_step, valid_bits, next_upper
                                        )) {
                                        window_overflow = true;
                                        break;
                                    }
                                    window_upper = next_upper;
                                }
                                if (window_overflow) {
                                    continue;
                                }

                                if (!component.flexible) {
                                    SerialTick occurrence{};
                                    if (!NextSerialTickOccurrence(
                                            component.begin_tick, valid_bits, state.end, occurrence
                                        )) {
                                        continue;
                                    }
                                    while (SerialTickLessEqual(occurrence, window_upper)) {
                                        if (!charge_topology_work(1)) {
                                            topology_limit_failed = true;
                                            return false;
                                        }
                                        SerialTick occurrence_end{};
                                        if (!AddSerialTickDelta(
                                                occurrence, component.duration, valid_bits, occurrence_end
                                            )) {
                                            break;
                                        }
                                        next_frontier.push_back({
                                            .begin = occurrence,
                                            .end   = occurrence_end,
                                        });
                                        if (occurrence.epoch == std::numeric_limits<std::uint64_t>::max()) {
                                            break;
                                        }
                                        ++occurrence.epoch;
                                    }
                                    continue;
                                }

                                const std::uint64_t begin_slack = maximum_duration - component.duration;
                                SerialTick          latest_envelope{};
                                if (!AddSerialTickDelta(
                                        window_upper, begin_slack, valid_bits, latest_envelope
                                    )) {
                                    continue;
                                }
                                SerialTick envelope_occurrence{};
                                if (!NextSerialTickOccurrence(
                                        component.begin_tick, valid_bits, state.end, envelope_occurrence
                                    )) {
                                    continue;
                                }
                                while (SerialTickLessEqual(envelope_occurrence, latest_envelope)) {
                                    if (!charge_topology_work(1)) {
                                        topology_limit_failed = true;
                                        return false;
                                    }
                                    SerialTick envelope_begin_floor{};
                                    if (SubtractSerialTickDelta(
                                            envelope_occurrence, begin_slack, valid_bits, envelope_begin_floor
                                        )) {
                                        const SerialTick feasible_begin = envelope_begin_floor < state.end ?
                                                                              state.end :
                                                                              envelope_begin_floor;
                                        const SerialTick feasible_end   = window_upper < envelope_occurrence ?
                                                                              window_upper :
                                                                              envelope_occurrence;
                                        SerialTick       next_end{};
                                        if (SerialTickLessEqual(feasible_begin, feasible_end) &&
                                            AddSerialTickDelta(
                                                envelope_occurrence, component.duration, valid_bits, next_end
                                            )) {
                                            next_frontier.push_back({
                                                .begin = feasible_end,
                                                .end   = next_end,
                                            });
                                        }
                                    }
                                    if (envelope_occurrence.epoch ==
                                        std::numeric_limits<std::uint64_t>::max()) {
                                        break;
                                    }
                                    ++envelope_occurrence.epoch;
                                }
                            }
                            if (next_frontier.empty()) {
                                return false;
                            }
                            std::sort(
                                next_frontier.begin(),
                                next_frontier.end(),
                                [](const RootSerialState& _left, const RootSerialState& _right) {
                                    if (_left.begin < _right.begin) {
                                        return false;
                                    }
                                    if (_right.begin < _left.begin) {
                                        return true;
                                    }
                                    return _left.end < _right.end;
                                }
                            );
                            std::vector<RootSerialState> pruned_frontier;
                            pruned_frontier.reserve(next_frontier.size());
                            for (const RootSerialState& state : next_frontier) {
                                if (!pruned_frontier.empty() && !((state.end) < pruned_frontier.back().end)) {
                                    continue;
                                }
                                pruned_frontier.push_back(state);
                            }
                            frontier = std::move(pruned_frontier);
                        }
                        return !frontier.empty();
                    };

                    bool used_split_plan     = false;
                    bool root_sequence_valid = validate_root_sequence(root_components, occupied_local_orders);
                    if (!root_sequence_valid && !topology_limit_failed && has_split_plan) {
                        std::vector<std::uint64_t> split_occupied_local_orders = occupied_local_orders;
                        split_occupied_local_orders.insert(
                            split_occupied_local_orders.end(),
                            used_split_slots.begin(),
                            used_split_slots.end()
                        );
                        std::sort(split_occupied_local_orders.begin(), split_occupied_local_orders.end());
                        root_sequence_valid =
                            validate_root_sequence(split_root_components, split_occupied_local_orders);
                        used_split_plan = root_sequence_valid;
                    }
                    if (topology_limit_failed) {
                        return false;
                    }
                    if (!root_sequence_valid) {
                        return fail_root_sequence();
                    }
                    if (used_split_plan) {
                        std::uint64_t required_parent_record_count = 0;
                        if (AddOverflow(
                                source->required_parent_record_count,
                                static_cast<std::uint64_t>(used_split_slots.size()),
                                required_parent_record_count
                            ) ||
                            required_parent_record_count > source->local_hole_count) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeParentInvalid,
                                "concrete GPU root partition exceeds its source local-order holes"
                            );
                            return false;
                        }
                        source->required_parent_record_count = required_parent_record_count;
                        const std::uint64_t frame_index =
                            scopes[static_cast<std::size_t>(source->first_scope)].frame_index;
                        if (frame_index == kInvalidSessionIndex || frame_index >= frames.size()) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::GpuScopeIdentityInvalid,
                                "partitioned GPU root has no owning frame"
                            );
                            return false;
                        }
                        frame_root_partition_ambiguous[static_cast<std::size_t>(frame_index)] = 1;
                    }
                }
            }

            if (!timing_topology_trusted) {
                std::size_t   observed_index              = static_cast<std::size_t>(source->first_scope);
                std::uint64_t hidden_ancestor_lower_bound = 0;
                std::map<std::uint32_t, std::uint32_t> required_prefix_depth_intervals;
                std::uint32_t                          exposed_prefix_depth                = 0;
                std::uint64_t                          required_prefix_depth_count         = 0;
                std::uint64_t                          matched_required_direct_depth_count = 0;
                const auto                             prefix_depth_was_required = [&](std::uint32_t _depth) {
                    const auto interval = required_prefix_depth_intervals.upper_bound(_depth);
                    if (interval == required_prefix_depth_intervals.begin()) {
                        return false;
                    }
                    const auto& candidate = *std::prev(interval);
                    return _depth >= candidate.first && _depth < candidate.second;
                };
                const auto prefix_direct_depth_count_in_range = [&](std::uint32_t _begin,
                                                                    std::uint32_t _end) {
                    return covered_depth_count_before(_end, prefix_covered_depth_tree) -
                           covered_depth_count_before(_begin, prefix_covered_depth_tree);
                };
                const auto add_required_prefix_range = [&](std::uint32_t _begin, std::uint32_t _end) {
                    if (_begin >= _end) {
                        return;
                    }

                    auto interval = required_prefix_depth_intervals.lower_bound(_begin);
                    if (interval != required_prefix_depth_intervals.begin()) {
                        auto previous = std::prev(interval);
                        if (previous->second >= _begin) {
                            interval = previous;
                        }
                    }

                    std::uint32_t merged_begin = _begin;
                    std::uint32_t merged_end   = _end;
                    std::uint32_t cursor       = _begin;
                    while (interval != required_prefix_depth_intervals.end() && interval->first <= merged_end
                    ) {
                        if (cursor < _end && cursor < interval->first) {
                            const std::uint32_t uncovered_end = std::min(_end, interval->first);
                            required_prefix_depth_count += static_cast<std::uint64_t>(uncovered_end - cursor);
                            matched_required_direct_depth_count +=
                                prefix_direct_depth_count_in_range(cursor, uncovered_end);
                        }
                        cursor       = std::max(cursor, interval->second);
                        merged_begin = std::min(merged_begin, interval->first);
                        merged_end   = std::max(merged_end, interval->second);
                        interval     = required_prefix_depth_intervals.erase(interval);
                    }
                    if (cursor < _end) {
                        required_prefix_depth_count += static_cast<std::uint64_t>(_end - cursor);
                        matched_required_direct_depth_count +=
                            prefix_direct_depth_count_in_range(cursor, _end);
                    }
                    required_prefix_depth_intervals.emplace(merged_begin, merged_end);
                };

                std::vector<std::uint32_t> observed_ancestor_depths;
                observed_ancestor_depths.reserve(std::min<std::size_t>(
                    static_cast<std::size_t>(options.limits.max_scope_depth),
                    source_end_index - static_cast<std::size_t>(source->first_scope)
                ));
                for (std::size_t index = contract_begin; index < contract_end; ++index) {
                    const MissingParentContract& contract                = missing_parent_contracts[index];
                    const std::size_t            parent_depth_coordinate = static_cast<std::size_t>(
                        std::lower_bound(
                            depth_coordinates.begin(), depth_coordinates.end(), contract.parent_depth
                        ) -
                        depth_coordinates.begin()
                    );
                    if (prefix_depth_covered[parent_depth_coordinate] == 0 &&
                        prefix_depth_was_required(contract.parent_depth)) {
                        ++matched_required_direct_depth_count;
                    }
                    cover_depth(contract.parent_depth, prefix_covered_depth_tree, prefix_depth_covered);

                    observed_ancestor_depths.clear();
                    if (timing_topology_trusted) {
                        std::uint64_t ancestor_index = contract.observed_ancestor_index;
                        while (ancestor_index != kInvalidSessionIndex) {
                            const GpuScopeRecord& ancestor = scopes[ancestor_index];
                            if (ancestor.depth < contract.parent_depth) {
                                observed_ancestor_depths.push_back(ancestor.depth);
                            }
                            ancestor_index = nearest_observed_timing_ancestor[ancestor_index];
                        }
                        std::sort(observed_ancestor_depths.begin(), observed_ancestor_depths.end());
                        observed_ancestor_depths.erase(
                            std::unique(observed_ancestor_depths.begin(), observed_ancestor_depths.end()),
                            observed_ancestor_depths.end()
                        );
                    } else {
                        while (observed_index < source_end_index &&
                               scopes[observed_index].local_order < contract.latest_parent_local_order) {
                            cover_depth(scopes[observed_index].depth, covered_depth_tree, depth_covered);
                            cover_depth(
                                scopes[observed_index].depth, prefix_covered_depth_tree, prefix_depth_covered
                            );
                            ++observed_index;
                        }
                    }

                    std::uint64_t covered_ancestor_depths =
                        covered_depth_count_before(contract.parent_depth, covered_depth_tree);
                    if (timing_topology_trusted) {
                        for (const std::uint32_t depth : observed_ancestor_depths) {
                            const std::size_t coordinate = static_cast<std::size_t>(
                                std::lower_bound(depth_coordinates.begin(), depth_coordinates.end(), depth) -
                                depth_coordinates.begin()
                            );
                            if (depth_covered[coordinate] == 0) {
                                ++covered_ancestor_depths;
                            }
                        }
                    }
                    const std::uint64_t hidden_ancestors =
                        static_cast<std::uint64_t>(contract.parent_depth) - covered_ancestor_depths;
                    hidden_ancestor_lower_bound = std::max(hidden_ancestor_lower_bound, hidden_ancestors);

                    if (timing_topology_trusted) {
                        std::uint32_t interval_begin = 0;
                        for (const std::uint32_t depth : observed_ancestor_depths) {
                            add_required_prefix_range(interval_begin, depth);
                            interval_begin = depth + 1;
                        }
                        add_required_prefix_range(interval_begin, contract.parent_depth);
                    } else if (contract.parent_depth > exposed_prefix_depth) {
                        std::uint32_t interval_begin = exposed_prefix_depth;
                        auto          coordinate     = std::lower_bound(
                            depth_coordinates.begin(), depth_coordinates.end(), exposed_prefix_depth
                        );
                        while (coordinate != depth_coordinates.end() && *coordinate < contract.parent_depth) {
                            const std::size_t coordinate_index =
                                static_cast<std::size_t>(coordinate - depth_coordinates.begin());
                            if (prefix_depth_covered[coordinate_index] != 0) {
                                add_required_prefix_range(interval_begin, *coordinate);
                                interval_begin = *coordinate + 1;
                            }
                            ++coordinate;
                        }
                        add_required_prefix_range(interval_begin, contract.parent_depth);
                        exposed_prefix_depth = contract.parent_depth;
                    }

                    const std::uint64_t parent_ordinal =
                        static_cast<std::uint64_t>(index - contract_begin + 1);
                    const std::uint64_t unmatched_required_prefix_depths =
                        required_prefix_depth_count - matched_required_direct_depth_count;
                    std::uint64_t       required_prefix_slots = 0;
                    const std::uint64_t available_local_order_slots =
                        contract.child_local_order_deadline - contract.observed_local_orders_before_deadline;
                    if (AddOverflow(
                            parent_ordinal, unmatched_required_prefix_depths, required_prefix_slots
                        ) ||
                        required_prefix_slots > available_local_order_slots) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeParentInvalid,
                            "missing GpuScope ancestry exceeds its local-order deadline"
                        );
                        return false;
                    }
                }

                const std::uint64_t direct_parent_count =
                    static_cast<std::uint64_t>(contract_end - contract_begin);
                hidden_ancestor_lower_bound = std::max(
                    hidden_ancestor_lower_bound,
                    required_prefix_depth_count - matched_required_direct_depth_count
                );
                std::uint64_t depth_required_parent_records = 0;
                if (AddOverflow(
                        direct_parent_count, hidden_ancestor_lower_bound, depth_required_parent_records
                    )) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "missing GpuScope ancestry count overflows"
                    );
                    return false;
                }
                source->required_parent_record_count = depth_required_parent_records;
                if (source->required_parent_record_count > source->local_hole_count) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeParentInvalid,
                        "missing GpuScope ancestry exceeds its source local-order holes"
                    );
                    return false;
                }
            }
            contract_begin = contract_end;
        }

        struct FrameLossEvidence {
            std::uint64_t frame_id{0};
            std::uint64_t local_hole_count{0};
            std::uint64_t required_parent_record_count{0};
        };
        std::vector<FrameLossEvidence> frame_loss_evidence;
        frame_loss_evidence.reserve(source_loss_evidence.size());
        for (const SourceLossEvidence& source : source_loss_evidence) {
            frame_loss_evidence.push_back({
                .frame_id                     = source.frame_id,
                .local_hole_count             = source.local_hole_count,
                .required_parent_record_count = source.required_parent_record_count,
            });
        }
        std::sort(
            frame_loss_evidence.begin(),
            frame_loss_evidence.end(),
            [](const FrameLossEvidence& _left, const FrameLossEvidence& _right) {
                return _left.frame_id < _right.frame_id;
            }
        );
        std::size_t frame_evidence_count = 0;
        for (std::size_t index = 0; index < frame_loss_evidence.size(); ++index) {
            if (frame_evidence_count == 0 || frame_loss_evidence[frame_evidence_count - 1].frame_id !=
                                                 frame_loss_evidence[index].frame_id) {
                frame_loss_evidence[frame_evidence_count++] = frame_loss_evidence[index];
                continue;
            }
            FrameLossEvidence& aggregate        = frame_loss_evidence[frame_evidence_count - 1];
            std::uint64_t      local_holes      = 0;
            std::uint64_t      required_parents = 0;
            if (AddOverflow(
                    aggregate.local_hole_count, frame_loss_evidence[index].local_hole_count, local_holes
                ) ||
                AddOverflow(
                    aggregate.required_parent_record_count,
                    frame_loss_evidence[index].required_parent_record_count,
                    required_parents
                )) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "GPU source loss lower bound overflows"
                );
                return false;
            }
            aggregate.local_hole_count             = local_holes;
            aggregate.required_parent_record_count = required_parents;
        }
        frame_loss_evidence.resize(frame_evidence_count);
        const auto find_frame_loss_evidence = [&](std::uint64_t _frame_id) {
            const auto found = std::lower_bound(
                frame_loss_evidence.begin(),
                frame_loss_evidence.end(),
                _frame_id,
                [](const FrameLossEvidence& _evidence, std::uint64_t _value) {
                    return _evidence.frame_id < _value;
                }
            );
            return found != frame_loss_evidence.end() && found->frame_id == _frame_id ?
                       &*found :
                       static_cast<const FrameLossEvidence*>(nullptr);
        };

        std::sort(missing_parent_keys.begin(), missing_parent_keys.end());
        missing_parent_keys.erase(
            std::unique(
                missing_parent_keys.begin(),
                missing_parent_keys.end(),
                [](const ScopeLookup& _left, const ScopeLookup& _right) {
                    return _left.frame_id == _right.frame_id && _left.scope_id == _right.scope_id;
                }
            ),
            missing_parent_keys.end()
        );
        std::sort(missing_frame_ids.begin(), missing_frame_ids.end());
        missing_frame_ids.erase(
            std::unique(missing_frame_ids.begin(), missing_frame_ids.end()), missing_frame_ids.end()
        );

        const auto missing_parent_count = [&](std::uint64_t _frame_id) {
            const ScopeLookup first{
                .frame_id    = _frame_id,
                .scope_id    = 0,
                .scope_index = 0,
            };
            const ScopeLookup last{
                .frame_id    = _frame_id,
                .scope_id    = std::numeric_limits<std::uint64_t>::max(),
                .scope_index = std::numeric_limits<std::uint64_t>::max(),
            };
            const auto begin =
                std::lower_bound(missing_parent_keys.begin(), missing_parent_keys.end(), first);
            const auto end = std::upper_bound(missing_parent_keys.begin(), missing_parent_keys.end(), last);
            return static_cast<std::uint64_t>(end - begin);
        };

        std::uint64_t required_gpu_drops     = _required_cpu_drops;
        const auto    add_required_gpu_drops = [&](std::uint64_t _count) {
            std::uint64_t next = 0;
            if (AddOverflow(required_gpu_drops, _count, next)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "minimum CPU/GPU record loss count overflows"
                );
                return false;
            }
            required_gpu_drops = next;
            return true;
        };

        for (std::size_t index = 0; index < frames.size(); ++index) {
            GpuFrameRecord&     frame  = frames[index];
            const std::uint64_t errors = frame_error_counts[index];
            const bool          observed_too_many =
                frame.scope_count > frame.admitted_scope_count || errors > frame.error_scope_count;
            const std::uint64_t observed_ready = errors <= frame.scope_count ? frame.scope_count - errors : 0;
            const std::uint64_t declared_ready = frame.admitted_scope_count - frame.error_scope_count;
            if (observed_too_many || observed_ready > declared_ready) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "GpuFrame Ready/Error totals exceed its declarations"
                );
                return false;
            }

            const std::uint64_t      missing_scopes          = frame.admitted_scope_count - frame.scope_count;
            const std::uint64_t      missing_parents         = missing_parent_count(frame.frame_id);
            const FrameLossEvidence* loss_evidence           = find_frame_loss_evidence(frame.frame_id);
            const std::uint64_t      required_parent_records = std::max(
                missing_parents, loss_evidence != nullptr ? loss_evidence->required_parent_record_count : 0
            );
            std::uint64_t explainable_local_holes = 0;
            if (AddOverflow(missing_scopes, frame.dropped_scope_count, explainable_local_holes)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "GpuFrame scope deficit accounting overflows"
                );
                return false;
            }
            if ((loss_evidence != nullptr && loss_evidence->local_hole_count > explainable_local_holes) ||
                required_parent_records > missing_scopes) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "GpuFrame declarations cannot explain its source local-order holes"
                );
                return false;
            }

            frame.export_missing_scope_count = missing_scopes;
            frame.materialization_complete   = missing_scopes == 0;
            if (frame.status == ProfileGpuFrameStatus::Complete) {
                if (!add_required_gpu_drops(missing_scopes)) {
                    return false;
                }
                if (missing_scopes != 0) {
                    ++session.impl_->summary.degraded_complete_gpu_frame_count;
                }
            } else if (!add_required_gpu_drops(required_parent_records)) {
                return false;
            }
        }

        if (!add_required_gpu_drops(static_cast<std::uint64_t>(missing_frame_ids.size()))) {
            return false;
        }
        for (const std::uint64_t frame_id : missing_frame_ids) {
            const FrameLossEvidence* loss_evidence           = find_frame_loss_evidence(frame_id);
            const std::uint64_t      required_parent_records = std::max(
                missing_parent_count(frame_id),
                loss_evidence != nullptr ? loss_evidence->required_parent_record_count : 0
            );
            if (!add_required_gpu_drops(required_parent_records)) {
                return false;
            }
        }
        if (session.impl_->summary.has_session_end) {
            const std::uint64_t total_drop_budget = session.impl_->summary.lost_record_count;
            if (required_gpu_drops > total_drop_budget) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "CPU/GPU deficits exceed the noticed allocated-record loss budget"
                );
                return false;
            }
        }

        std::uint64_t previous_root_index = kInvalidSessionIndex;
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            const GpuScopeRecord& scope = scopes[index];
            if (scope.parent_scope_id != 0 || scope.frame_index == kInvalidSessionIndex) {
                continue;
            }
            const GpuFrameRecord& frame = frames[scope.frame_index];
            if (frame.status != ProfileGpuFrameStatus::Complete || !frame.materialization_complete) {
                previous_root_index = kInvalidSessionIndex;
                continue;
            }

            if (previous_root_index != kInvalidSessionIndex) {
                const GpuScopeRecord& previous = scopes[previous_root_index];
                const bool            same_source =
                    previous.frame_id == scope.frame_id && previous.logical_queue == scope.logical_queue &&
                    previous.native_queue_id == scope.native_queue_id &&
                    previous.family_id == scope.family_id && previous.source_order == scope.source_order;
                if (same_source) {
                    if (previous.valid_bits != scope.valid_bits ||
                        !NearlyEqualDuration(previous.tick_period_ns, scope.tick_period_ns)) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "serial GPU roots disagree on their timestamp domain"
                        );
                        return false;
                    }
                    const std::uint64_t begin_distance =
                        (scope.begin_tick - previous.begin_tick) & TimestampMask(scope.valid_bits);
                    const std::uint64_t previous_duration =
                        TimestampDelta(previous.begin_tick, previous.end_tick, previous.valid_bits);
                    const std::uint64_t half_range = scope.valid_bits == 64 ?
                                                         (std::uint64_t{1} << 63) :
                                                         (std::uint64_t{1} << (scope.valid_bits - 1));
                    if (begin_distance < previous_duration || begin_distance >= half_range) {
                        Fail(
                            SessionLoadStatus::ProtocolViolation,
                            SessionErrorCode::GpuScopeTimingInvalid,
                            "serial GPU roots overlap or move backward in their timestamp domain"
                        );
                        return false;
                    }
                }
            }
            previous_root_index = index;
        }

        for (std::size_t index = 0; index < scopes.size(); ++index) {
            const GpuScopeRecord& scope = scopes[index];
            if (scope.frame_index == kInvalidSessionIndex) {
                continue;
            }
            GpuFrameRecord& frame = frames[scope.frame_index];
            if (frame.status != ProfileGpuFrameStatus::Complete) {
                continue;
            }

            const double child_duration = direct_child_duration[index];
            if (!std::isfinite(child_duration) ||
                !DurationContains(scope.total_duration_ns, child_duration)) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeTimingInvalid,
                    "GpuScope direct-child durations exceed the parent duration"
                );
                return false;
            }
            if (frame.materialization_complete) {
                const double expected_exclusive = std::max(0.0, scope.total_duration_ns - child_duration);
                if (!NearlyEqualDuration(scope.exclusive_duration_ns, expected_exclusive)) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuScopeTimingInvalid,
                        "GpuScope exclusive duration disagrees with its complete child set"
                    );
                    return false;
                }
            } else if (!DurationContains(
                           scope.total_duration_ns, scope.exclusive_duration_ns + child_duration
                       )) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuScopeTimingInvalid,
                    "partial GpuScope evidence already exceeds the parent duration"
                );
                return false;
            }
        }
        for (std::size_t index = 0; index < frames.size(); ++index) {
            GpuFrameRecord& frame         = frames[index];
            frame.timing_topology_trusted = frame.status == ProfileGpuFrameStatus::Complete &&
                                            frame.materialization_complete &&
                                            frame_root_partition_ambiguous[index] == 0;
        }
        return true;
    }

    [[nodiscard]] bool BuildIndexes(bool _forensic) {
        std::sort(record_sequences.begin(), record_sequences.end());
        if (!record_sequences.empty() && record_sequences.front() == 0) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSequenceInvalid,
                "profile record sequence zero is reserved"
            );
            return false;
        }
        if (std::adjacent_find(record_sequences.begin(), record_sequences.end()) != record_sequences.end()) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSequenceDuplicate,
                "profile record sequence is duplicated"
            );
            return false;
        }
        // ProfileDump v3 does not encode the last reserved sequence.
        // SessionEnd::records_dropped mixes pre-reservation attempts with
        // sequence-backed losses, while value validation/encoding can consume a
        // sequence without incrementing that aggregate. Therefore neither
        // written+dropped nor written+noticed is a valid sequence frontier.
        if (!ValidateLossSequenceAllocation()) {
            return false;
        }

        // Only a Loss notice proves that an allocated record sequence was
        // dropped. Unnotified SessionEnd attempts remain useful telemetry, but
        // cannot justify a missing CPU/GPU record.
        const bool    allow_missing      = _forensic || session.impl_->summary.lost_record_count != 0;
        std::uint64_t required_cpu_drops = 0;
        std::vector<RecordSequenceDemand> cpu_sequence_demands;
        if (!BuildCpuTracks(allow_missing, required_cpu_drops, cpu_sequence_demands)) {
            return false;
        }
        if (session.impl_->summary.has_session_end &&
            required_cpu_drops > session.impl_->summary.lost_record_count) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::CpuScopeParentMissing,
                "CPU hierarchy deficits exceed the noticed allocated-record loss budget"
            );
            return false;
        }
        if (!_forensic && !ValidateCpuLossCompatibility(cpu_sequence_demands)) {
            return false;
        }
        if (!BuildGpuDomainsAndTracks() || !BuildGpuTopology(allow_missing, required_cpu_drops)) {
            return false;
        }
        session.impl_->valid = true;
        return true;
    }

    void ReleaseWorkingState() noexcept {
        std::vector<std::uint8_t>{}.swap(packet_bytes);
        std::unordered_map<std::uint64_t, SchemaEntry>{}.swap(schemas);
        std::unordered_map<std::string_view, SessionStringId>{}.swap(interned_strings);
        std::vector<std::uint64_t>{}.swap(record_sequences);
    }

    [[nodiscard]] SessionLoadResult Feed(std::span<const std::uint8_t> _bytes) noexcept {
        if (!IsReading() || _bytes.empty()) {
            return result;
        }
        try {
            std::uint64_t next_input_bytes = 0;
            if (AddOverflow(
                    result.input_bytes, static_cast<std::uint64_t>(_bytes.size()), next_input_bytes
                ) ||
                next_input_bytes > options.limits.max_input_bytes) {
                Fail(
                    SessionLoadStatus::LimitExceeded,
                    SessionErrorCode::LimitExceeded,
                    "profile input byte limit exceeded",
                    DecodeStatus::LimitExceeded,
                    SessionLimitKind::InputBytes,
                    result.input_bytes
                );
                return result;
            }
            const std::uint64_t feed_begin_offset = result.input_bytes;
            result.input_bytes                    = next_input_bytes;

            std::size_t cursor = 0;
            while (cursor < _bytes.size() && IsReading()) {
                if (saw_session_end) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::TrailingDataAfterSessionEnd,
                        "bytes appear after SessionEnd",
                        DecodeStatus::Ok,
                        SessionLimitKind::None,
                        feed_begin_offset + cursor
                    );
                    break;
                }

                if (expected_packet_bytes == 0) {
                    const std::size_t header_remaining = kPacketHeaderBytes - packet_bytes.size();
                    const std::size_t copy_bytes       = std::min(header_remaining, _bytes.size() - cursor);
                    packet_bytes.insert(
                        packet_bytes.end(), _bytes.begin() + cursor, _bytes.begin() + cursor + copy_bytes
                    );
                    cursor += copy_bytes;
                    if (packet_bytes.size() < kPacketHeaderBytes) {
                        continue;
                    }

                    PacketView         header_view{};
                    std::size_t        consumed = 0;
                    const DecodeStatus header_status =
                        DecodePacket(packet_bytes, options.limits.codec, header_view, consumed);
                    if (header_status != DecodeStatus::NeedMoreData && header_status != DecodeStatus::Ok) {
                        FailCodec(
                            header_status,
                            SessionErrorCode::CodecHeaderInvalid,
                            "profile packet header failed validation"
                        );
                        break;
                    }
                    const std::uint32_t payload_bytes     = ReadU32LittleEndian(packet_bytes, 12);
                    std::size_t         packet_wire_bytes = 0;
                    if (!Detail::TryPacketWireBytes(payload_bytes, packet_wire_bytes)) {
                        FailCodec(
                            DecodeStatus::PayloadTooLarge,
                            SessionErrorCode::CodecHeaderInvalid,
                            "profile packet size exceeds the addressable range"
                        );
                        break;
                    }
                    if (static_cast<std::uint64_t>(packet_wire_bytes) > options.limits.max_input_bytes) {
                        Fail(
                            SessionLoadStatus::LimitExceeded,
                            SessionErrorCode::LimitExceeded,
                            "profile packet cannot fit within the input byte limit",
                            DecodeStatus::Ok,
                            SessionLimitKind::InputBytes
                        );
                        break;
                    }
                    expected_packet_bytes = packet_wire_bytes;
                    packet_bytes.reserve(expected_packet_bytes);
                }

                const std::size_t packet_remaining = expected_packet_bytes - packet_bytes.size();
                const std::size_t copy_bytes       = std::min(packet_remaining, _bytes.size() - cursor);
                packet_bytes.insert(
                    packet_bytes.end(), _bytes.begin() + cursor, _bytes.begin() + cursor + copy_bytes
                );
                cursor += copy_bytes;
                if (packet_bytes.size() != expected_packet_bytes) {
                    continue;
                }

                PacketView         packet{};
                std::size_t        consumed = 0;
                const DecodeStatus decode =
                    DecodePacket(packet_bytes, options.limits.codec, packet, consumed);
                if (decode != DecodeStatus::Ok || consumed != expected_packet_bytes) {
                    FailCodec(
                        decode == DecodeStatus::Ok ? DecodeStatus::MalformedPayload : decode,
                        SessionErrorCode::CodecPacketInvalid,
                        "profile packet failed validation"
                    );
                    break;
                }
                if (!ProcessPacket(packet)) {
                    break;
                }

                result.valid_prefix_bytes += expected_packet_bytes;
                packet_bytes.clear();
                expected_packet_bytes = 0;
            }
        } catch (const std::bad_alloc&) {
            Fail(
                SessionLoadStatus::ResourceExhausted,
                SessionErrorCode::ResourceAllocationFailed,
                "profile consumer allocation failed"
            );
        } catch (...) {
            Fail(
                SessionLoadStatus::ResourceExhausted,
                SessionErrorCode::UnexpectedFailure,
                "profile consumer rejected an unexpected allocation failure"
            );
        }
        return result;
    }

    [[nodiscard]] SessionLoadResult Finish() noexcept {
        if (!IsReading()) {
            return result;
        }
        try {
            if (!saw_session_begin) {
                Fail(
                    SessionLoadStatus::CorruptData,
                    SessionErrorCode::SessionBeginMissing,
                    "profile input has no complete SessionBegin",
                    DecodeStatus::NeedMoreData
                );
                return result;
            }

            bool forensic = false;
            if (!packet_bytes.empty()) {
                result.incomplete_reason = packet_bytes.size() < kPacketHeaderBytes ?
                                               SessionIncompleteReason::TruncatedHeader :
                                               SessionIncompleteReason::TruncatedPayload;
                if (!options.allow_forensic_truncation) {
                    Fail(
                        SessionLoadStatus::CorruptData,
                        SessionErrorCode::TruncatedPacket,
                        "profile input ends inside a packet",
                        DecodeStatus::NeedMoreData
                    );
                    return result;
                }
                forensic = true;
            } else if (!saw_session_end) {
                result.incomplete_reason = SessionIncompleteReason::MissingSessionEnd;
                if (!options.allow_forensic_truncation) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::SessionEndMissing,
                        "profile input has no SessionEnd"
                    );
                    return result;
                }
                forensic = true;
            }

            if (!BuildIndexes(forensic)) {
                return result;
            }
            if (forensic) {
                result.incomplete_byte_offset  = result.valid_prefix_bytes;
                result.incomplete_packet_index = expected_packet_index;
            }
            ReleaseWorkingState();
            result.status = forensic ? SessionLoadStatus::ForensicTruncated : SessionLoadStatus::Complete;
            diagnostic =
                forensic ? "valid profile prefix materialized for forensic use" : "profile session loaded";
        } catch (const std::bad_alloc&) {
            Fail(
                SessionLoadStatus::ResourceExhausted,
                SessionErrorCode::ResourceAllocationFailed,
                "profile index allocation failed"
            );
        } catch (...) {
            Fail(
                SessionLoadStatus::ResourceExhausted,
                SessionErrorCode::UnexpectedFailure,
                "profile index construction failed"
            );
        }
        return result;
    }

    SessionLoadOptions options{};
    SessionLoadResult  result{};
    const char*        diagnostic{"profile session reader is accepting input"};
    ProfileSession     session{};

    std::vector<std::uint8_t> packet_bytes{};
    std::size_t               expected_packet_bytes{0};
    std::uint64_t             expected_packet_index{0};
    bool                      saw_session_begin{false};
    bool                      saw_session_end{false};

    std::unordered_map<std::uint64_t, SchemaEntry>        schemas{};
    std::unordered_map<std::string_view, SessionStringId> interned_strings{};
    std::vector<std::uint64_t>                            record_sequences{};
    std::uint64_t                                         schema_bytes{0};
    std::uint64_t                                         string_bytes{0};
    std::uint64_t                                         topology_work_items{0};
};

ProfileSessionReader::ProfileSessionReader(const SessionLoadOptions& _options) noexcept {
    try {
        impl_ = new (std::nothrow) Impl(_options);
    } catch (...) {
        impl_ = nullptr;
    }
}

ProfileSessionReader::~ProfileSessionReader() {
    delete impl_;
}

ProfileSessionReader::ProfileSessionReader(ProfileSessionReader&& _other) noexcept :
    impl_(std::exchange(_other.impl_, nullptr)) {}

ProfileSessionReader& ProfileSessionReader::operator=(ProfileSessionReader&& _other) noexcept {
    if (this != &_other) {
        delete impl_;
        impl_ = std::exchange(_other.impl_, nullptr);
    }
    return *this;
}

SessionLoadResult ProfileSessionReader::Feed(std::span<const std::uint8_t> _bytes) noexcept {
    return impl_ != nullptr ? impl_->Feed(_bytes) : ResourceExhaustedResult();
}

SessionLoadResult ProfileSessionReader::Finish() noexcept {
    return impl_ != nullptr ? impl_->Finish() : ResourceExhaustedResult();
}

const SessionLoadResult& ProfileSessionReader::Result() const noexcept {
    static const SessionLoadResult exhausted = ResourceExhaustedResult();
    return impl_ != nullptr ? impl_->result : exhausted;
}

std::string_view ProfileSessionReader::DiagnosticMessage() const noexcept {
    return impl_ != nullptr ? std::string_view(impl_->diagnostic) :
                              std::string_view("profile session reader allocation failed");
}

const ProfileSession& ProfileSessionReader::Session() const noexcept {
    static const ProfileSession empty{};
    return impl_ != nullptr && impl_->result.HasUsableSession() ? impl_->session : empty;
}

ProfileSession ProfileSessionReader::TakeSession() noexcept {
    if (impl_ == nullptr || !impl_->result.HasUsableSession()) {
        return {};
    }
    return std::move(impl_->session);
}

SessionLoadResult LoadProfileSessionFile(
    const std::filesystem::path& _path,
    const SessionLoadOptions&    _options,
    ProfileSession&              _output
) noexcept {
    if (_path.empty()) {
        SessionLoadResult result{};
        result.status     = SessionLoadStatus::InvalidArgument;
        result.error_code = SessionErrorCode::InvalidArgument;
        return result;
    }

    try {
        std::ifstream stream(_path, std::ios::binary | std::ios::ate);
        if (!stream.is_open()) {
            SessionLoadResult result{};
            result.status     = SessionLoadStatus::OpenFailed;
            result.error_code = SessionErrorCode::FileOpenFailed;
            return result;
        }
        const std::streamoff snapshot = stream.tellg();
        if (snapshot < 0) {
            SessionLoadResult result{};
            result.status     = SessionLoadStatus::ReadFailed;
            result.error_code = SessionErrorCode::FileReadFailed;
            return result;
        }
        const std::uint64_t snapshot_bytes = static_cast<std::uint64_t>(snapshot);
        if (snapshot_bytes > _options.limits.max_input_bytes) {
            SessionLoadResult result{};
            result.status      = SessionLoadStatus::LimitExceeded;
            result.error_code  = SessionErrorCode::LimitExceeded;
            result.limit_kind  = SessionLimitKind::InputBytes;
            result.input_bytes = snapshot_bytes;
            return result;
        }
        stream.seekg(0);
        if (!stream.good()) {
            SessionLoadResult result{};
            result.status     = SessionLoadStatus::ReadFailed;
            result.error_code = SessionErrorCode::FileReadFailed;
            return result;
        }

        ProfileSessionReader reader(_options);
        if (reader.Result().status != SessionLoadStatus::Reading) {
            return reader.Result();
        }

        std::array<std::uint8_t, kReadBlockBytes> block{};
        std::uint64_t                             remaining = snapshot_bytes;
        while (remaining != 0) {
            const std::size_t request =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block.size()));
            stream.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(request));
            const std::streamsize read = stream.gcount();
            if (read != static_cast<std::streamsize>(request)) {
                SessionLoadResult result = reader.Result();
                result.status            = SessionLoadStatus::ReadFailed;
                result.error_code        = SessionErrorCode::FileReadFailed;
                result.input_bytes       = snapshot_bytes - remaining +
                                     static_cast<std::uint64_t>(std::max<std::streamsize>(read, 0));
                return result;
            }

            const SessionLoadResult fed =
                reader.Feed(std::span<const std::uint8_t>(block.data(), static_cast<std::size_t>(read)));
            if (fed.IsTerminal()) {
                return fed;
            }
            remaining -= static_cast<std::uint64_t>(read);
        }

        const SessionLoadResult finished = reader.Finish();
        if (finished.HasUsableSession()) {
            _output = reader.TakeSession();
        }
        return finished;
    } catch (const std::bad_alloc&) {
        return ResourceExhaustedResult();
    } catch (...) {
        SessionLoadResult result{};
        result.status     = SessionLoadStatus::ReadFailed;
        result.error_code = SessionErrorCode::UnexpectedFailure;
        return result;
    }
}

} // namespace Moer::ProfileDump
