#ifndef MOER_ENGINE_PROFILE_TIMELINE_INDEX_H
#define MOER_ENGINE_PROFILE_TIMELINE_INDEX_H

#include "profile_consumer/ProfileSession.h"

#include <cstdint>
#include <limits>
#include <span>
#include <stop_token>

namespace Moer::ProfileDump {

inline constexpr std::uint32_t kInvalidTimelineAxis = std::numeric_limits<std::uint32_t>::max();

enum class TimelineAxisKind : std::uint8_t {
    CpuSteadyClock = 0,
    GpuPhysicalTimestampDomain,
};

enum class TimelineIndexBuildStatus : std::uint8_t {
    Ready = 0,
    InvalidArgument,
    LimitExceeded,
    ProtocolViolation,
    ResourceExhausted,
    Cancelled,
};

enum class TimelineIndexLimitKind : std::uint8_t {
    None = 0,
    CpuScopes,
    GpuFrameSlices,
    GpuTimelineScopes,
    LogicalBytes,
    TransientBuildBytes,
};

enum class TimelineQualityFlag : std::uint32_t {
    ForensicTruncated   = std::uint32_t{1} << 0,
    LostRecords         = std::uint32_t{1} << 1,
    UnnotifiedDrops     = std::uint32_t{1} << 2,
    CpuOrphans          = std::uint32_t{1} << 3,
    GpuOrphans          = std::uint32_t{1} << 4,
    DegradedGpuFrames   = std::uint32_t{1} << 5,
    IncompleteGpuFrames = std::uint32_t{1} << 6,
    InvalidGpuFrames    = std::uint32_t{1} << 7,
    GpuScopeErrors      = std::uint32_t{1} << 8,
    UntrustedGpuTiming  = std::uint32_t{1} << 9,
    UnknownRecords      = std::uint32_t{1} << 10,
};

[[nodiscard]] inline constexpr std::uint32_t TimelineQualityBit(TimelineQualityFlag _flag) noexcept {
    return static_cast<std::uint32_t>(_flag);
}

struct TimelineIndexLimits {
    std::uint64_t max_cpu_scopes{4'000'000};
    std::uint64_t max_gpu_frame_slices{4'000'000};
    std::uint64_t max_gpu_timeline_scopes{4'000'000};
    // Retained index payload only. ProfileSession storage and transient build
    // allocations have their own independent limits.
    std::uint64_t max_logical_bytes{512ull * 1024 * 1024};
    // Conservative peak for temporary GPU planning and cancellable sort
    // workspaces. It excludes the immutable ProfileSession and retained index.
    std::uint64_t max_transient_build_bytes{512ull * 1024 * 1024};
};

struct TimelineIndexBuildResult {
    TimelineIndexBuildStatus status{TimelineIndexBuildStatus::InvalidArgument};
    TimelineIndexLimitKind   limit_kind{TimelineIndexLimitKind::None};
    std::uint64_t            logical_bytes{0};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == TimelineIndexBuildStatus::Ready;
    }

    friend bool operator==(const TimelineIndexBuildResult&, const TimelineIndexBuildResult&) = default;
};

struct TimelineIndexBuildControl {
    std::stop_token stop_token{};
    // Linear build loops sample cancellation at this interval. Potentially
    // large sorts are split into bounded runs no larger than this interval
    // (and an internal responsiveness cap), with cancellable merge passes.
    // Allocator and standard-library work inside one bounded run cannot be
    // interrupted. Zero is normalized to one.
    std::uint64_t cancellation_check_interval{4096};
    // Optional deterministic cooperative budget. Exactly this many charged
    // work items may run; the next checkpoint reports Cancelled and follows
    // the same atomic-output contract as stop_token. This can cap background
    // work even when the caller has no timer thread.
    std::uint64_t max_work_items_before_cancel{std::numeric_limits<std::uint64_t>::max()};
};

// CPU and every physical GPU timestamp domain are separate axes. ProfileDump
// v3 contains no CPU/GPU or cross-GPU-domain calibration, so consumers must
// never align distinct axis indices as if their zero points were comparable.
struct TimelineAxis {
    TimelineAxisKind kind{TimelineAxisKind::CpuSteadyClock};
    std::uint32_t    source_domain_index{kInvalidTimelineAxis};
    std::uint32_t    native_queue_id{0};
    std::uint32_t    family_id{0};
    std::uint32_t    logical_queue_mask{0};
    std::uint32_t    valid_bits{0};
    double           unit_period_ns{1.0};
    bool             timing_available{false};
    // This describes the physical domain's timestamp capability, not the
    // topology trust of any particular frame.
    bool timing_capability_trusted{false};
    bool calibrated_to_cpu{false};
    bool calibrated_to_other_gpu_domains{false};

    friend bool operator==(const TimelineAxis&, const TimelineAxis&) = default;
};

struct CpuTimelineScopeRef {
    std::uint64_t source_scope_index{kInvalidSessionIndex};
    // Relative to ProfileSessionSummary::cpu_begin_ns. Integer subtraction is
    // performed before presentation converts values to floating point.
    std::uint64_t begin_ns{0};
    std::uint64_t end_ns{0};

    friend bool operator==(const CpuTimelineScopeRef&, const CpuTimelineScopeRef&) = default;
};

struct CpuTimelineTrackIndex {
    std::uint32_t source_track_index{0};
    std::uint32_t axis_index{0};
    std::uint64_t first_scope{0};
    std::uint64_t scope_count{0};
    std::uint64_t begin_ns{0};
    std::uint64_t end_ns{0};
    std::uint32_t max_depth{0};

    friend bool operator==(const CpuTimelineTrackIndex&, const CpuTimelineTrackIndex&) = default;
};

struct GpuTimelineFrameRef {
    std::uint64_t frame_id{0};
    std::uint64_t source_frame_index{kInvalidSessionIndex};

    friend bool operator==(const GpuTimelineFrameRef&, const GpuTimelineFrameRef&) = default;
};

// One frame-local presentation domain for a physical GPU timestamp axis.
// Offsets from distinct axis-frame records are intentionally incomparable,
// including two frames on the same physical axis.
struct GpuTimelineAxisFrame {
    std::uint64_t         frame_id{0};
    std::uint64_t         source_frame_index{kInvalidSessionIndex};
    std::uint32_t         axis_index{kInvalidTimelineAxis};
    std::uint64_t         origin_tick{0};
    std::uint64_t         extent_ticks{0};
    std::uint64_t         ready_scope_count{0};
    std::uint64_t         error_scope_count{0};
    ProfileGpuFrameStatus frame_status{ProfileGpuFrameStatus::Invalid};
    bool                  has_frame_record{false};
    bool                  timing_available{false};
    bool                  timing_capability_trusted{false};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};

    friend bool operator==(const GpuTimelineAxisFrame&, const GpuTimelineAxisFrame&) = default;
};

struct GpuTimelineScopeRef {
    std::uint64_t source_scope_index{kInvalidSessionIndex};
    // Integer offsets from GpuTimelineAxisFrame::origin_tick. Multiply only
    // after subtraction by TimelineAxis::unit_period_ns for presentation.
    std::uint64_t begin_tick_offset{0};
    std::uint64_t end_tick_offset{0};

    friend bool operator==(const GpuTimelineScopeRef&, const GpuTimelineScopeRef&) = default;
};

struct GpuTimelineFrameSlice {
    std::uint64_t         frame_id{0};
    std::uint64_t         source_frame_index{kInvalidSessionIndex};
    std::uint64_t         axis_frame_index{kInvalidSessionIndex};
    std::uint64_t         first_scope{0};
    std::uint64_t         scope_count{0};
    std::uint64_t         first_timeline_scope{0};
    std::uint64_t         timeline_scope_count{0};
    std::uint64_t         error_scope_count{0};
    ProfileGpuFrameStatus frame_status{ProfileGpuFrameStatus::Invalid};
    bool                  has_frame_record{false};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};

    friend bool operator==(const GpuTimelineFrameSlice&, const GpuTimelineFrameSlice&) = default;
};

struct GpuTimelineTrackIndex {
    std::uint32_t source_track_index{0};
    std::uint32_t axis_index{kInvalidTimelineAxis};
    std::uint64_t first_frame_slice{0};
    std::uint64_t frame_slice_count{0};

    friend bool operator==(const GpuTimelineTrackIndex&, const GpuTimelineTrackIndex&) = default;
};

struct ProfileTimelineQuality {
    std::uint32_t flags{0};
    std::uint64_t lost_record_count{0};
    std::uint64_t unnotified_drop_count{0};
    std::uint64_t orphan_cpu_scope_count{0};
    std::uint64_t orphan_gpu_scope_count{0};
    std::uint64_t degraded_complete_gpu_frame_count{0};
    std::uint64_t incomplete_gpu_frame_count{0};
    std::uint64_t invalid_gpu_frame_count{0};
    std::uint64_t error_gpu_scope_count{0};
    std::uint64_t untrusted_gpu_frame_count{0};
    std::uint64_t unknown_record_count{0};

    [[nodiscard]] bool Has(TimelineQualityFlag _flag) const noexcept {
        return (flags & TimelineQualityBit(_flag)) != 0;
    }

    [[nodiscard]] bool Clean() const noexcept {
        return flags == 0;
    }

    friend bool operator==(const ProfileTimelineQuality&, const ProfileTimelineQuality&) = default;
};

struct TimelineOverlapQueryResult {
    std::uint64_t written{0};
    bool          truncated{false};
    bool          valid{false};
};

class ProfileTimelineIndex final {
public:
    ProfileTimelineIndex() noexcept = default;
    ~ProfileTimelineIndex();

    ProfileTimelineIndex(const ProfileTimelineIndex&)            = delete;
    ProfileTimelineIndex& operator=(const ProfileTimelineIndex&) = delete;
    ProfileTimelineIndex(ProfileTimelineIndex&& _other) noexcept;
    ProfileTimelineIndex& operator=(ProfileTimelineIndex&& _other) noexcept;

    // The index stores source indices rather than owning session strings or
    // records. Keep the exact immutable ProfileSession alive while using it.
    [[nodiscard]] bool Valid() const noexcept;
    // Tests exact immutable-model identity and remains true if the owning
    // ProfileSession object is moved (its model allocation stays stable).
    [[nodiscard]] bool                            Matches(const ProfileSession& _session) const noexcept;
    [[nodiscard]] const TimelineIndexBuildResult& BuildResult() const noexcept;
    [[nodiscard]] const ProfileTimelineQuality&   Quality() const noexcept;

    [[nodiscard]] std::span<const TimelineAxis>          Axes() const noexcept;
    [[nodiscard]] std::span<const CpuTimelineTrackIndex> CpuTracks() const noexcept;
    [[nodiscard]] std::span<const CpuTimelineScopeRef>   CpuScopes() const noexcept;
    [[nodiscard]] std::span<const GpuTimelineFrameRef>   GpuFrames() const noexcept;
    [[nodiscard]] std::span<const GpuTimelineTrackIndex> GpuTracks() const noexcept;
    [[nodiscard]] std::span<const GpuTimelineFrameSlice> GpuFrameSlices() const noexcept;
    [[nodiscard]] std::span<const GpuTimelineAxisFrame>  GpuAxisFrames() const noexcept;
    [[nodiscard]] std::span<const GpuTimelineScopeRef>   GpuTimelineScopes() const noexcept;

    // Returns source CpuScope indices in deterministic begin-time order.
    // The query is output-bounded: it stops after finding the first match that
    // does not fit in _output and reports truncated instead of scanning the
    // remainder of a very large visible range.
    [[nodiscard]] TimelineOverlapQueryResult QueryCpuOverlaps(
        std::uint32_t            _track_index,
        std::uint64_t            _view_begin_ns,
        std::uint64_t            _view_end_ns,
        std::span<std::uint64_t> _output
    ) const noexcept;

    // Viewer-oriented variant that returns the already-normalized timeline
    // references as well as their source indices. This remains output-bounded
    // and avoids rescanning a track or reconstructing relative timing in UI
    // code.
    [[nodiscard]] TimelineOverlapQueryResult QueryCpuTimelineOverlaps(
        std::uint32_t                  _track_index,
        std::uint64_t                  _view_begin_ns,
        std::uint64_t                  _view_end_ns,
        std::span<CpuTimelineScopeRef> _output
    ) const noexcept;

    [[nodiscard]] const GpuTimelineFrameRef* FindGpuFrame(std::uint64_t _frame_id) const noexcept;
    [[nodiscard]] const GpuTimelineFrameSlice*
    FindGpuFrameSlice(std::uint32_t _track_index, std::uint64_t _frame_id) const noexcept;
    [[nodiscard]] const GpuTimelineAxisFrame*
    FindGpuAxisFrame(std::uint32_t _axis_index, std::uint64_t _frame_id) const noexcept;

    // Returns ready source GpuScope indices in frame-local tick-offset order.
    // Error scopes remain discoverable through the source frame slice but do
    // not participate in a timing query.
    [[nodiscard]] TimelineOverlapQueryResult QueryGpuOverlaps(
        std::uint32_t            _track_index,
        std::uint64_t            _frame_id,
        std::uint64_t            _view_begin_ticks,
        std::uint64_t            _view_end_ticks,
        std::span<std::uint64_t> _output
    ) const noexcept;

    // Returns frame-local, wrap-normalized timing references. Distinct
    // axis/frame records remain incomparable; callers must pair these offsets
    // with the exact GpuTimelineAxisFrame selected for the query.
    [[nodiscard]] TimelineOverlapQueryResult QueryGpuTimelineOverlaps(
        std::uint32_t                  _track_index,
        std::uint64_t                  _frame_id,
        std::uint64_t                  _view_begin_ticks,
        std::uint64_t                  _view_end_ticks,
        std::span<GpuTimelineScopeRef> _output
    ) const noexcept;

private:
    struct Impl;
    Impl* impl_{nullptr};

    friend TimelineIndexBuildResult
    BuildProfileTimelineIndex(const ProfileSession&, const SessionLoadResult&, const TimelineIndexLimits&, ProfileTimelineIndex&, const TimelineIndexBuildControl&) noexcept;
};

// _session and _load_result must be the paired outputs of the same load.
// _output is move-replaced only after a complete, bounded index has been
// constructed. A failed rebuild leaves the previous usable index untouched.
[[nodiscard]] TimelineIndexBuildResult BuildProfileTimelineIndex(
    const ProfileSession&            _session,
    const SessionLoadResult&         _load_result,
    const TimelineIndexLimits&       _limits,
    ProfileTimelineIndex&            _output,
    const TimelineIndexBuildControl& _control = {}
) noexcept;

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_TIMELINE_INDEX_H
