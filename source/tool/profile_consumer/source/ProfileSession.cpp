#include "profile_consumer/ProfileSession.h"

#include "profile/ProfileDumpTemplates.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <new>
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

    [[nodiscard]] bool BuildCpuTracks(bool _allow_missing) {
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
            for (std::size_t index = track_begin; index < track_end; ++index) {
                CpuScopeRecord& scope = scopes[index];
                scope.track_index     = track_index;

                while (!active.empty()) {
                    const CpuScopeRecord& candidate = scopes[active.back()];
                    if (candidate.end_ns >= scope.end_ns && candidate.begin_ns <= scope.begin_ns &&
                        candidate.depth < scope.depth) {
                        break;
                    }
                    active.pop_back();
                }
                while (!active.empty() && scopes[active.back()].depth >= scope.depth) {
                    active.pop_back();
                }

                if (scope.depth != 0) {
                    auto parent =
                        std::find_if(active.rbegin(), active.rend(), [&](std::uint64_t _candidate_index) {
                            const CpuScopeRecord& candidate = scopes[_candidate_index];
                            return candidate.depth == scope.depth - 1 &&
                                   candidate.begin_ns <= scope.begin_ns && candidate.end_ns >= scope.end_ns;
                        });
                    if (parent == active.rend()) {
                        ++session.impl_->summary.orphan_cpu_scope_count;
                        if (!_allow_missing) {
                            Fail(
                                SessionLoadStatus::ProtocolViolation,
                                SessionErrorCode::CpuScopeParentMissing,
                                "CpuScope hierarchy has a missing parent"
                            );
                            return false;
                        }
                    } else {
                        scope.parent_index = *parent;
                    }
                }
                active.push_back(index);
            }
            track_begin = track_end;
        }
        return true;
    }

    [[nodiscard]] bool BuildGpuDomainsAndTracks() {
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
                if (!domain.has_ready_timestamps) {
                    domain.has_ready_timestamps = true;
                    domain.valid_bits           = scope.valid_bits;
                    domain.tick_period_ns       = scope.tick_period_ns;
                } else if (domain.valid_bits != scope.valid_bits ||
                           domain.tick_period_ns != scope.tick_period_ns) {
                    Fail(
                        SessionLoadStatus::ProtocolViolation,
                        SessionErrorCode::GpuDomainConflict,
                        "one GPU timestamp domain reports conflicting timing capabilities"
                    );
                    return false;
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

    [[nodiscard]] bool BuildGpuTopology(bool _allow_missing) {
        auto& frames = session.impl_->gpu_frames;
        auto& scopes = session.impl_->gpu_scopes;
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

        std::vector<std::uint64_t> frame_error_counts(frames.size(), 0);
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
            if (frame == frames.end() || frame->frame_id != scope.frame_id) {
                orphan = true;
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
                }
            }
            if (orphan) {
                ++session.impl_->summary.orphan_gpu_scope_count;
            }
        }

        for (std::size_t index = 0; index < frames.size(); ++index) {
            const GpuFrameRecord& frame             = frames[index];
            const bool            observed_too_many = frame.scope_count > frame.admitted_scope_count ||
                                           frame_error_counts[index] > frame.error_scope_count;
            const bool intact_frame_mismatch = !_allow_missing &&
                                               frame.status != ProfileGpuFrameStatus::Invalid &&
                                               (frame.scope_count != frame.admitted_scope_count ||
                                                frame_error_counts[index] != frame.error_scope_count);
            if (observed_too_many || intact_frame_mismatch) {
                Fail(
                    SessionLoadStatus::ProtocolViolation,
                    SessionErrorCode::GpuFrameTotalsMismatch,
                    "GpuFrame scope totals do not match its records"
                );
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool BuildIndexes(bool _forensic) {
        std::sort(record_sequences.begin(), record_sequences.end());
        if (std::adjacent_find(record_sequences.begin(), record_sequences.end()) != record_sequences.end()) {
            Fail(
                SessionLoadStatus::ProtocolViolation,
                SessionErrorCode::RecordSequenceDuplicate,
                "profile record sequence is duplicated"
            );
            return false;
        }

        const bool allow_missing = _forensic || session.impl_->summary.lost_record_count != 0 ||
                                   session.impl_->summary.unnotified_drop_count != 0;
        if (!BuildCpuTracks(allow_missing) || !BuildGpuDomainsAndTracks() ||
            !BuildGpuTopology(allow_missing)) {
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
                    const std::uint32_t payload_bytes = ReadU32LittleEndian(packet_bytes, 12);
                    expected_packet_bytes = kPacketHeaderBytes + static_cast<std::size_t>(payload_bytes);
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
