#ifndef MOER_ENGINE_PROFILE_SESSION_H
#define MOER_ENGINE_PROFILE_SESSION_H

#include "profile/ProfileDumpCodec.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stop_token>
#include <string_view>
#include <type_traits>

namespace Moer::ProfileDump {

using SessionStringId = std::uint32_t;

inline constexpr SessionStringId kInvalidSessionString = std::numeric_limits<SessionStringId>::max();
inline constexpr std::uint64_t   kInvalidSessionIndex  = std::numeric_limits<std::uint64_t>::max();

namespace Detail {

// Compute the full wire size before converting the untrusted uint32 payload
// length to the address-space-sized reader counter. The generic Size keeps the
// 32-bit boundary independently testable on a 64-bit build.
template<typename Size>
[[nodiscard]] constexpr bool TryPacketWireBytes(std::uint32_t _payload_bytes, Size& _result) noexcept {
    static_assert(std::is_unsigned_v<Size>);
    constexpr std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<Size>::max());
    const std::uint64_t     total =
        static_cast<std::uint64_t>(kPacketHeaderBytes) + static_cast<std::uint64_t>(_payload_bytes);
    if (total > maximum) {
        return false;
    }
    _result = static_cast<Size>(total);
    return true;
}

} // namespace Detail

enum class SessionLoadStatus : std::uint8_t {
    Reading = 0,
    Complete,
    ForensicTruncated,
    InvalidArgument,
    OpenFailed,
    ReadFailed,
    UnsupportedVersion,
    CorruptData,
    ProtocolViolation,
    LimitExceeded,
    ResourceExhausted,
    Cancelled,
};

enum class SessionIncompleteReason : std::uint8_t {
    None = 0,
    MissingSessionEnd,
    TruncatedHeader,
    TruncatedPayload,
};

// Stable machine-readable reason for terminal load failures. A successful
// Complete/ForensicTruncated result always reports None.
enum class SessionErrorCode : std::uint16_t {
    None = 0,
    InvalidArgument,
    FileOpenFailed,
    FileReadFailed,
    ResourceAllocationFailed,
    UnexpectedFailure,
    LimitExceeded,
    CodecHeaderInvalid,
    CodecPacketInvalid,
    SessionBeginMissing,
    SessionBeginDuplicate,
    SessionEndMissing,
    SessionEndTotalsMismatch,
    PacketIndexMismatch,
    TrailingDataAfterSessionEnd,
    TruncatedPacket,
    SchemaDecodeInvalid,
    SchemaConflict,
    RecordPayloadInvalid,
    RecordSchemaUnregistered,
    RecordSequenceDuplicate,
    RecordSequenceInvalid,
    LossPayloadInvalid,
    LossTotalsOverflow,
    CpuScopePayloadInvalid,
    CpuScopeTimeInvalid,
    CpuScopeParentMissing,
    GpuFramePayloadInvalid,
    GpuFrameStatusInvalid,
    GpuFrameDuplicate,
    GpuFrameTotalsMismatch,
    GpuScopePayloadInvalid,
    GpuScopeIdentityInvalid,
    GpuScopeQueueInvalid,
    GpuScopeStatusInvalid,
    GpuScopeTimingInvalid,
    GpuDomainConflict,
    GpuScopeDuplicate,
    GpuScopeFrameMissing,
    GpuScopeRootDepthInvalid,
    GpuScopeParentMissing,
    GpuScopeParentInvalid,
    CpuScopeTopologyInvalid,
    Cancelled,
};

enum class SessionLimitKind : std::uint8_t {
    None = 0,
    Codec,
    InputBytes,
    Packets,
    Schemas,
    SchemaBytes,
    Records,
    LossNotices,
    CpuScopes,
    GpuFrames,
    GpuScopes,
    CpuTracks,
    GpuTracks,
    GpuDomains,
    UniqueStrings,
    StringBytes,
    LogicalModelBytes,
    ScopeDepth,
    TopologyWorkItems,
    TopologyFlowEdges,
    TransientMaterializationBytes,
};

enum class KnownProfileSchema : std::uint8_t {
    Unknown = 0,
    CpuScopeV1,
    GpuFrameV1,
    GpuScopeV2,
};

enum class ProfileGpuFrameStatus : std::uint32_t {
    Complete = 0,
    Incomplete,
    Invalid,
};

enum class ProfileGpuScopeStatus : std::uint32_t {
    Ready = 0,
    Error,
};

enum class ProfileLogicalQueue : std::uint32_t {
    Graphics = 0,
    Compute,
    Copy,
};

[[nodiscard]] inline constexpr std::uint32_t ProfileLogicalQueueBit(ProfileLogicalQueue _queue) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(_queue);
}

struct SessionLimits {
    CodecLimits codec{};

    std::uint64_t max_input_bytes{16ull * 1024 * 1024 * 1024};
    std::uint64_t max_packets{16'000'000};
    std::uint32_t max_schemas{4096};
    std::uint64_t max_schema_bytes{16ull * 1024 * 1024};
    std::uint64_t max_records{8'000'000};
    std::uint64_t max_loss_notices{1'000'000};

    std::uint64_t max_cpu_scopes{4'000'000};
    std::uint64_t max_gpu_frames{500'000};
    std::uint64_t max_gpu_scopes{4'000'000};
    std::uint32_t max_cpu_tracks{4096};
    std::uint32_t max_gpu_tracks{1024};
    std::uint32_t max_gpu_domains{256};
    std::uint32_t max_scope_depth{1024};
    // Covers ordinary O(N log N) indexing as well as exceptional topology
    // reconstruction. This default accommodates practical offline captures,
    // but the shared work budget may reject adversarial inputs before the
    // independent record/scope count limits. Callers handling untrusted
    // captures should lower it to their latency budget.
    std::uint64_t max_topology_work_items{1'000'000'000};
    // Counts forward residual-network edges before allocation. Reverse edges
    // are checked separately for addressability and allocated as part of each
    // admitted forward edge.
    std::uint64_t max_topology_flow_edges{1'000'000};

    std::uint32_t max_unique_strings{1'000'000};
    std::uint64_t max_string_bytes{256ull * 1024 * 1024};
    // Logical retained-model estimate, not a process RSS or allocator-capacity
    // guarantee. Transient decode/index storage remains bounded by the count
    // and codec limits above.
    std::uint64_t max_logical_model_bytes{1024ull * 1024 * 1024};
    // Peak scratch payload admitted by one cancellable materialization sort.
    // This is independent of retained logical_model_bytes.
    std::uint64_t max_transient_materialization_bytes{1024ull * 1024 * 1024};
};

struct SessionLoadOptions {
    SessionLimits limits{};

    // Only a clean prefix ending at the last complete packet may be exposed.
    // CRC, schema, protocol, and semantic failures are never downgraded.
    bool allow_forensic_truncation{false};
};

struct SessionLoadControl {
    std::stop_token stop_token{};
    // Linear decode/materialization loops sample cancellation at this
    // interval. Potentially large sorts are split into bounded runs no larger
    // than this interval (and an internal responsiveness cap), with
    // cancellable merge passes. Zero is normalized to one. Decoding and
    // processing one complete packet is an atomic, non-interruptible boundary;
    // the default CodecLimits cap its payload at 1 MiB, while raising that
    // limit also raises the worst-case cancellation tail latency.
    std::uint64_t cancellation_check_interval{4096};
    // Optional deterministic cooperative budget. Exactly N charged work
    // items are admitted; cancellation occurs before the first item that
    // would exceed N, or at the next zero-work checkpoint after exhaustion.
    // Cancellation follows the same no-publication contract as stop_token.
    std::uint64_t max_work_items_before_cancel{std::numeric_limits<std::uint64_t>::max()};
};

struct SessionLoadResult {
    SessionLoadStatus       status{SessionLoadStatus::Reading};
    SessionIncompleteReason incomplete_reason{SessionIncompleteReason::None};
    SessionErrorCode        error_code{SessionErrorCode::None};
    SessionLimitKind        limit_kind{SessionLimitKind::None};
    DecodeStatus            codec_status{DecodeStatus::Ok};

    std::uint64_t error_byte_offset{kInvalidSessionIndex};
    std::uint64_t error_packet_index{kInvalidSessionIndex};
    std::uint64_t incomplete_byte_offset{kInvalidSessionIndex};
    std::uint64_t incomplete_packet_index{kInvalidSessionIndex};
    std::uint64_t input_bytes{0};
    std::uint64_t valid_prefix_bytes{0};
    std::uint64_t packet_count{0};

    [[nodiscard]] bool HasUsableSession() const noexcept {
        return status == SessionLoadStatus::Complete || status == SessionLoadStatus::ForensicTruncated;
    }

    [[nodiscard]] bool IsTerminal() const noexcept {
        return status != SessionLoadStatus::Reading;
    }
};

struct ProfileSchemaFieldInfo {
    SessionStringId name{kInvalidSessionString};
    FieldType       type{FieldType::Bool};
};

struct ProfileSchemaInfo {
    std::uint64_t      hash{0};
    SessionStringId    name{kInvalidSessionString};
    SessionStringId    event_type{kInvalidSessionString};
    std::uint32_t      schema_version{0};
    EventKind          kind{EventKind::Scope};
    Channel            channel{Channel::CpuThread};
    KnownProfileSchema known_schema{KnownProfileSchema::Unknown};
    std::uint64_t      first_field{0};
    std::uint32_t      field_count{0};
    std::uint64_t      record_count{0};
};

struct ProfileLossRecord {
    std::uint64_t first_sequence{0};
    std::uint64_t last_sequence{0};
    std::uint64_t record_count{0};
    std::uint64_t value_bytes{0};
    std::uint32_t reason_mask{0};
};

struct CpuScopeRecord {
    std::uint64_t   sequence{0};
    std::uint64_t   thread_id{0};
    SessionStringId name{kInvalidSessionString};
    std::uint64_t   begin_ns{0};
    std::uint64_t   end_ns{0};
    std::uint32_t   depth{0};
    std::uint32_t   track_index{0};
    std::uint64_t   parent_index{kInvalidSessionIndex};
};

// CPU scopes use the producer process's steady-clock nanosecond domain. The
// SessionBegin Unix timestamp is metadata, not an absolute clock anchor.
struct CpuTrack {
    std::uint64_t thread_id{0};
    std::uint64_t first_scope{0};
    std::uint64_t scope_count{0};
};

struct GpuFrameRecord {
    std::uint64_t         sequence{0};
    std::uint64_t         frame_id{0};
    ProfileGpuFrameStatus status{ProfileGpuFrameStatus::Invalid};
    bool                  valid{false};
    std::uint64_t         admitted_scope_count{0};
    std::uint64_t         dropped_scope_count{0};
    std::uint64_t         error_scope_count{0};
    SessionStringId       reason{kInvalidSessionString};
    std::uint64_t         scope_count{0};
    std::uint64_t         export_missing_scope_count{0};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};
};

// One physical timestamp domain. Different logical roles may share it when
// they resolve to the same native queue/family; GpuTrack keeps those roles
// distinct for presentation. No CPU/GPU or cross-domain calibration exists.
struct GpuTimestampDomain {
    std::uint32_t native_queue_id{0};
    std::uint32_t family_id{0};
    // Bit positions are defined by ProfileLogicalQueue.
    std::uint32_t logical_queue_mask{0};
    bool          has_ready_timestamps{false};
    bool          timing_capability_trusted{false};
    std::uint32_t valid_bits{0};
    double        tick_period_ns{0.0};
    std::uint64_t ready_scope_count{0};
    std::uint64_t error_scope_count{0};
};

struct GpuTrack {
    ProfileLogicalQueue logical_queue{ProfileLogicalQueue::Graphics};
    std::uint32_t       native_queue_id{0};
    std::uint32_t       family_id{0};
    std::uint32_t       domain_index{0};
    std::uint64_t       first_scope{0};
    std::uint64_t       scope_count{0};
};

struct GpuScopeRecord {
    std::uint64_t sequence{0};
    std::uint64_t frame_id{0};
    std::uint64_t scope_id{0};
    std::uint64_t parent_scope_id{0};
    std::uint64_t source_order{0};
    std::uint64_t local_order{0};

    ProfileLogicalQueue logical_queue{ProfileLogicalQueue::Graphics};
    std::uint32_t       native_queue_id{0};
    std::uint32_t       family_id{0};
    std::uint32_t       domain_index{0};
    std::uint32_t       track_index{0};
    std::uint64_t       frame_index{kInvalidSessionIndex};
    std::uint64_t       parent_index{kInvalidSessionIndex};

    SessionStringId       name{kInvalidSessionString};
    ProfileGpuScopeStatus status{ProfileGpuScopeStatus::Error};
    std::uint64_t         begin_tick{0};
    std::uint64_t         end_tick{0};
    std::uint32_t         valid_bits{0};
    double                tick_period_ns{0.0};
    double                total_duration_ns{0.0};
    double                exclusive_duration_ns{0.0};
    std::uint32_t         depth{0};
    SessionStringId       error_reason{kInvalidSessionString};
};

struct ProfileSessionSummary {
    std::uint64_t generation{0};
    std::uint64_t started_unix_ns{0};
    bool          has_session_end{false};
    std::uint64_t session_end_records_written{0};
    std::uint64_t session_end_records_dropped{0};
    std::uint64_t unnotified_drop_count{0};

    std::uint64_t packet_count{0};
    std::uint64_t schema_packet_count{0};
    std::uint64_t unique_schema_count{0};
    std::uint64_t record_count{0};
    std::uint64_t loss_notice_count{0};

    std::uint64_t cpu_scope_count{0};
    std::uint64_t gpu_frame_count{0};
    std::uint64_t gpu_scope_count{0};
    std::uint64_t unknown_record_count{0};

    std::uint64_t complete_gpu_frame_count{0};
    std::uint64_t degraded_complete_gpu_frame_count{0};
    std::uint64_t incomplete_gpu_frame_count{0};
    std::uint64_t invalid_gpu_frame_count{0};
    std::uint64_t ready_gpu_scope_count{0};
    std::uint64_t error_gpu_scope_count{0};
    std::uint64_t orphan_cpu_scope_count{0};
    std::uint64_t orphan_gpu_scope_count{0};

    std::uint64_t lost_record_count{0};
    std::uint64_t lost_value_bytes{0};
    std::uint32_t loss_reason_mask{0};

    bool          has_cpu_range{false};
    std::uint64_t cpu_begin_ns{0};
    std::uint64_t cpu_end_ns{0};
    // Estimated logical bytes retained by the immutable session model.
    std::uint64_t logical_model_bytes{0};
};

class ProfileSession {
public:
    ProfileSession() noexcept = default;
    ~ProfileSession();

    ProfileSession(const ProfileSession&)            = delete;
    ProfileSession& operator=(const ProfileSession&) = delete;
    ProfileSession(ProfileSession&& _other) noexcept;
    ProfileSession& operator=(ProfileSession&& _other) noexcept;

    // Returned spans and string views remain valid until this session is
    // destroyed or move-assigned. The model is immutable after a successful
    // reader Finish/LoadProfileSessionFile call.
    [[nodiscard]] bool                                    Valid() const noexcept;
    [[nodiscard]] const ProfileSessionSummary&            Summary() const noexcept;
    [[nodiscard]] std::span<const ProfileSchemaInfo>      Schemas() const noexcept;
    [[nodiscard]] std::span<const ProfileSchemaFieldInfo> SchemaFields() const noexcept;
    [[nodiscard]] std::span<const ProfileLossRecord>      Losses() const noexcept;
    [[nodiscard]] std::span<const CpuScopeRecord>         CpuScopes() const noexcept;
    [[nodiscard]] std::span<const CpuTrack>               CpuTracks() const noexcept;
    [[nodiscard]] std::span<const GpuFrameRecord>         GpuFrames() const noexcept;
    [[nodiscard]] std::span<const GpuScopeRecord>         GpuScopes() const noexcept;
    [[nodiscard]] std::span<const GpuTrack>               GpuTracks() const noexcept;
    [[nodiscard]] std::span<const GpuTimestampDomain>     GpuDomains() const noexcept;
    [[nodiscard]] std::string_view                        String(SessionStringId _id) const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};

    friend class ProfileSessionReader;
};

class ProfileSessionReader final {
public:
    explicit ProfileSessionReader(const SessionLoadOptions& _options = {}) noexcept;
    ProfileSessionReader(const SessionLoadOptions& _options, const SessionLoadControl& _control) noexcept;
    ~ProfileSessionReader();

    ProfileSessionReader(const ProfileSessionReader&)            = delete;
    ProfileSessionReader& operator=(const ProfileSessionReader&) = delete;
    ProfileSessionReader(ProfileSessionReader&& _other) noexcept;
    ProfileSessionReader& operator=(ProfileSessionReader&& _other) noexcept;

    // Feed accepts arbitrary chunk boundaries and retains at most one bounded
    // wire packet in addition to the explicitly limited session model. Seeing
    // SessionEnd does not publish Complete: only Finish establishes EOF, so a
    // later chunk containing trailing bytes cannot be silently ignored.
    [[nodiscard]] SessionLoadResult Feed(std::span<const std::uint8_t> _bytes) noexcept;
    [[nodiscard]] SessionLoadResult Finish() noexcept;

    [[nodiscard]] const SessionLoadResult& Result() const noexcept;
    [[nodiscard]] std::string_view         DiagnosticMessage() const noexcept;
    [[nodiscard]] const ProfileSession&    Session() const noexcept;
    [[nodiscard]] ProfileSession           TakeSession() noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};
};

// The file size is snapshotted once. Output is move-replaced only for a
// Complete or explicitly accepted ForensicTruncated result.
[[nodiscard]] SessionLoadResult LoadProfileSessionFile(
    const std::filesystem::path& _path,
    const SessionLoadOptions&    _options,
    ProfileSession&              _output
) noexcept;

[[nodiscard]] SessionLoadResult LoadProfileSessionFile(
    const std::filesystem::path& _path,
    const SessionLoadOptions&    _options,
    ProfileSession&              _output,
    const SessionLoadControl&    _control
) noexcept;

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_SESSION_H
