#ifndef MOER_ENGINE_PROFILE_TIMELINE_INDEX_H
#define MOER_ENGINE_PROFILE_TIMELINE_INDEX_H

#include "misc/NumericLimits.h"
#include "profile_consumer/ProfileSession.h"

#include "misc/Types.h"
#include <span>
#include <stop_token>

namespace Moer::ProfileDump {

inline constexpr uint32 kInvalidTimelineAxis = MaxValue<uint32>;

enum class TimelineAxisKind : uint8 {
    CpuSteadyClock = 0,
    GpuPhysicalTimestampDomain,
};

enum class TimelineIndexBuildStatus : uint8 {
    Ready = 0,
    InvalidArgument,
    LimitExceeded,
    ProtocolViolation,
    ResourceExhausted,
    Cancelled,
};

enum class TimelineIndexLimitKind : uint8 {
    None = 0,
    CpuScopes,
    GpuFrameSlices,
    GpuTimelineScopes,
    LogicalBytes,
    TransientBuildBytes,
};

enum class TimelineQualityFlag : uint32 {
    ForensicTruncated   = uint32{1} << 0,
    LostRecords         = uint32{1} << 1,
    UnnotifiedDrops     = uint32{1} << 2,
    CpuOrphans          = uint32{1} << 3,
    GpuOrphans          = uint32{1} << 4,
    DegradedGpuFrames   = uint32{1} << 5,
    IncompleteGpuFrames = uint32{1} << 6,
    InvalidGpuFrames    = uint32{1} << 7,
    GpuScopeErrors      = uint32{1} << 8,
    UntrustedGpuTiming  = uint32{1} << 9,
    UnknownRecords      = uint32{1} << 10,
};

[[nodiscard]] inline constexpr uint32 TimelineQualityBit(TimelineQualityFlag _flag) noexcept {
    return static_cast<uint32>(_flag);
}

struct TimelineIndexLimits {
    uint64 max_cpu_scopes{4'000'000};
    uint64 max_gpu_frame_slices{4'000'000};
    uint64 max_gpu_timeline_scopes{4'000'000};
    // Retained index payload only. ProfileSession storage and transient build
    // allocations have their own independent limits.
    uint64 max_logical_bytes{512ull * 1024 * 1024};
    // Conservative peak for temporary GPU planning and cancellable sort
    // workspaces. It excludes the immutable ProfileSession and retained index.
    uint64 max_transient_build_bytes{512ull * 1024 * 1024};
};

struct TimelineIndexBuildResult {
    TimelineIndexBuildStatus status{TimelineIndexBuildStatus::InvalidArgument};
    TimelineIndexLimitKind   limit_kind{TimelineIndexLimitKind::None};
    uint64                   logical_bytes{0};

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
    uint64 cancellation_check_interval{4096};
    // Optional deterministic cooperative budget. Exactly this many charged
    // work items may run; the next checkpoint reports Cancelled and follows
    // the same atomic-output contract as stop_token. This can cap background
    // work even when the caller has no timer thread.
    uint64 max_work_items_before_cancel{MaxValue<uint64>};
};

// CPU and every physical GPU timestamp domain are separate axes. ProfileDump
// v3 contains no CPU/GPU or cross-GPU-domain calibration, so consumers must
// never align distinct axis indices as if their zero points were comparable.
struct TimelineAxis {
    TimelineAxisKind kind{TimelineAxisKind::CpuSteadyClock};
    uint32           source_domain_index{kInvalidTimelineAxis};
    uint32           native_queue_id{0};
    uint32           family_id{0};
    uint32           logical_queue_mask{0};
    uint32           valid_bits{0};
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
    uint64 source_scope_index{kInvalidSessionIndex};
    // Relative to ProfileSessionSummary::cpu_begin_ns. Integer subtraction is
    // performed before presentation converts values to floating point.
    uint64 begin_ns{0};
    uint64 end_ns{0};

    friend bool operator==(const CpuTimelineScopeRef&, const CpuTimelineScopeRef&) = default;
};

struct CpuTimelineTrackIndex {
    uint32 source_track_index{0};
    uint32 axis_index{0};
    uint64 first_scope{0};
    uint64 scope_count{0};
    uint64 begin_ns{0};
    uint64 end_ns{0};
    uint32 max_depth{0};

    friend bool operator==(const CpuTimelineTrackIndex&, const CpuTimelineTrackIndex&) = default;
};

struct GpuTimelineFrameRef {
    uint64 frame_id{0};
    uint64 source_frame_index{kInvalidSessionIndex};

    friend bool operator==(const GpuTimelineFrameRef&, const GpuTimelineFrameRef&) = default;
};

// One frame-local presentation domain for a physical GPU timestamp axis.
// Offsets from distinct axis-frame records are intentionally incomparable,
// including two frames on the same physical axis.
struct GpuTimelineAxisFrame {
    uint64                frame_id{0};
    uint64                source_frame_index{kInvalidSessionIndex};
    uint32                axis_index{kInvalidTimelineAxis};
    uint64                origin_tick{0};
    uint64                extent_ticks{0};
    uint64                ready_scope_count{0};
    uint64                error_scope_count{0};
    ProfileGpuFrameStatus frame_status{ProfileGpuFrameStatus::Invalid};
    bool                  has_frame_record{false};
    bool                  timing_available{false};
    bool                  timing_capability_trusted{false};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};

    friend bool operator==(const GpuTimelineAxisFrame&, const GpuTimelineAxisFrame&) = default;
};

struct GpuTimelineScopeRef {
    uint64 source_scope_index{kInvalidSessionIndex};
    // Integer offsets from GpuTimelineAxisFrame::origin_tick. Multiply only
    // after subtraction by TimelineAxis::unit_period_ns for presentation.
    uint64 begin_tick_offset{0};
    uint64 end_tick_offset{0};

    friend bool operator==(const GpuTimelineScopeRef&, const GpuTimelineScopeRef&) = default;
};

struct GpuTimelineFrameSlice {
    uint64                frame_id{0};
    uint64                source_frame_index{kInvalidSessionIndex};
    uint64                axis_frame_index{kInvalidSessionIndex};
    uint64                first_scope{0};
    uint64                scope_count{0};
    uint64                first_timeline_scope{0};
    uint64                timeline_scope_count{0};
    uint64                error_scope_count{0};
    ProfileGpuFrameStatus frame_status{ProfileGpuFrameStatus::Invalid};
    bool                  has_frame_record{false};
    bool                  materialization_complete{false};
    bool                  timing_topology_trusted{false};

    friend bool operator==(const GpuTimelineFrameSlice&, const GpuTimelineFrameSlice&) = default;
};

struct GpuTimelineTrackIndex {
    uint32 source_track_index{0};
    uint32 axis_index{kInvalidTimelineAxis};
    uint64 first_frame_slice{0};
    uint64 frame_slice_count{0};

    friend bool operator==(const GpuTimelineTrackIndex&, const GpuTimelineTrackIndex&) = default;
};

struct ProfileTimelineQuality {
    uint32 flags{0};
    uint64 lost_record_count{0};
    uint64 unnotified_drop_count{0};
    uint64 orphan_cpu_scope_count{0};
    uint64 orphan_gpu_scope_count{0};
    uint64 degraded_complete_gpu_frame_count{0};
    uint64 incomplete_gpu_frame_count{0};
    uint64 invalid_gpu_frame_count{0};
    uint64 error_gpu_scope_count{0};
    uint64 untrusted_gpu_frame_count{0};
    uint64 unknown_record_count{0};

    [[nodiscard]] bool Has(TimelineQualityFlag _flag) const noexcept {
        return (flags & TimelineQualityBit(_flag)) != 0;
    }

    [[nodiscard]] bool Clean() const noexcept {
        return flags == 0;
    }

    friend bool operator==(const ProfileTimelineQuality&, const ProfileTimelineQuality&) = default;
};

struct TimelineOverlapQueryResult {
    uint64        written{0};
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
        uint32            _track_index,
        uint64            _view_begin_ns,
        uint64            _view_end_ns,
        std::span<uint64> _output
    ) const noexcept;

    // Viewer-oriented variant that returns the already-normalized timeline
    // references as well as their source indices. This remains output-bounded
    // and avoids rescanning a track or reconstructing relative timing in UI
    // code.
    [[nodiscard]] TimelineOverlapQueryResult QueryCpuTimelineOverlaps(
        uint32                         _track_index,
        uint64                         _view_begin_ns,
        uint64                         _view_end_ns,
        std::span<CpuTimelineScopeRef> _output
    ) const noexcept;

    [[nodiscard]] const GpuTimelineFrameRef* FindGpuFrame(uint64 _frame_id) const noexcept;
    [[nodiscard]] const GpuTimelineFrameSlice*
    FindGpuFrameSlice(uint32 _track_index, uint64 _frame_id) const noexcept;
    [[nodiscard]] const GpuTimelineAxisFrame*
    FindGpuAxisFrame(uint32 _axis_index, uint64 _frame_id) const noexcept;

    // Returns ready source GpuScope indices in frame-local tick-offset order.
    // Error scopes remain discoverable through the source frame slice but do
    // not participate in a timing query.
    [[nodiscard]] TimelineOverlapQueryResult QueryGpuOverlaps(
        uint32            _track_index,
        uint64            _frame_id,
        uint64            _view_begin_ticks,
        uint64            _view_end_ticks,
        std::span<uint64> _output
    ) const noexcept;

    // Returns frame-local, wrap-normalized timing references. Distinct
    // axis/frame records remain incomparable; callers must pair these offsets
    // with the exact GpuTimelineAxisFrame selected for the query.
    [[nodiscard]] TimelineOverlapQueryResult QueryGpuTimelineOverlaps(
        uint32                         _track_index,
        uint64                         _frame_id,
        uint64                         _view_begin_ticks,
        uint64                         _view_end_ticks,
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
