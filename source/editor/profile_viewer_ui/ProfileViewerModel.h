#pragma once

#include "profile_consumer/ProfileDocument.h"

#include "misc/Types.h"
#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace Moer {

inline constexpr std::size_t   kProfileViewerGpuViewportCapacity  = 256;
inline constexpr uint32        kProfileViewerGpuRenderableAxisMax = 256;

// Maps a bounded rational position onto [begin, end] without converting the
// uint64_t domain to floating point. Invalid fractions map to begin.
[[nodiscard]] uint64 ProfileViewerMapFraction(
    uint64 _begin,
    uint64 _end,
    uint32 _numerator,
    uint32 _denominator
) noexcept;

struct ProfileViewerGpuFrameChoice {
    uint64        frame_id{0};
    bool          has_frame_record{false};

    friend bool operator==(const ProfileViewerGpuFrameChoice&, const ProfileViewerGpuFrameChoice&) = default;
};

// The timeline index stores GpuFrame records by frame ID and physical-axis
// frames by (axis, frame). These allocation-free searches expose the sorted
// union of every recorded frame plus orphan timelines on viewer-renderable
// physical axes [1, kProfileViewerGpuRenderableAxisMax], without materializing
// or sorting a second large catalog.
[[nodiscard]] std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAtOrAfter(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept;
[[nodiscard]] std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAfter(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept;
[[nodiscard]] std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameBefore(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept;
[[nodiscard]] std::optional<ProfileViewerGpuFrameChoice> FindProfileViewerGpuFrameAtOrBefore(
    std::span<const ProfileDump::GpuTimelineFrameRef>  _recorded_frames,
    std::span<const ProfileDump::GpuTimelineAxisFrame> _axis_frames,
    uint64                                             _frame_id
) noexcept;

struct ProfileViewerViewport {
    bool          valid{false};
    uint64        domain_end{0};
    uint64        view_begin{0};
    uint64        view_end{0};

    friend bool operator==(const ProfileViewerViewport&, const ProfileViewerViewport&) = default;
};

struct ProfileViewerGpuViewportKey {
    uint32 axis_index{ProfileDump::kInvalidTimelineAxis};
    uint64 frame_id{0};

    friend bool operator==(const ProfileViewerGpuViewportKey&, const ProfileViewerGpuViewportKey&) = default;
};

enum class EProfileViewerSelectionKind : uint8 {
    None = 0,
    CpuScope,
    GpuScope,
};

struct ProfileViewerSelection {
    EProfileViewerSelectionKind kind{EProfileViewerSelectionKind::None};
    uint64                      published_generation{ProfileDump::kInvalidProfileDocumentGeneration};
    uint64                      source_scope_index{ProfileDump::kInvalidSessionIndex};
    uint64                      timeline_track_index{ProfileDump::kInvalidSessionIndex};
    uint32                      axis_index{ProfileDump::kInvalidTimelineAxis};
    uint64                      frame_id{0};
    uint64                      begin{0};
    uint64                      end{0};

    [[nodiscard]] bool Valid() const noexcept;

    friend bool operator==(const ProfileViewerSelection&, const ProfileViewerSelection&) = default;
};

enum class EProfileViewerPublicationUpdate : uint8 {
    Unchanged = 0,
    Changed,
    Invalid,
};

// Pure editor-side navigation state for immutable ProfileDocument publications.
// It deliberately retains no document pointer, span, string_view, or source
// address. The caller resolves stored integer identities against the current
// publication each frame.
class ProfileViewerModel final {
public:
    [[nodiscard]] EProfileViewerPublicationUpdate
    InspectSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot) const noexcept;
    [[nodiscard]] EProfileViewerPublicationUpdate
    ObserveSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot) noexcept;

    [[nodiscard]] uint64 PublishedGeneration() const noexcept;

    [[nodiscard]] ProfileViewerViewport CpuViewport() const noexcept;
    [[nodiscard]] bool                  FitCpu(uint64 _domain_end) noexcept;
    [[nodiscard]] bool                  ZoomCpu(
        uint32 _anchor_numerator,
        uint32 _anchor_denominator,
        uint32 _scale_numerator,
        uint32 _scale_denominator
    ) noexcept;
    [[nodiscard]] bool PanCpu(int64 _delta) noexcept;
    [[nodiscard]] bool FocusCpu(uint64 _begin, uint64 _end, uint64 _margin) noexcept;

    [[nodiscard]] std::optional<ProfileViewerViewport> FindGpuViewport(ProfileViewerGpuViewportKey _key
    ) const noexcept;
    [[nodiscard]] bool                                 FitGpu(ProfileViewerGpuViewportKey _key, uint64 _domain_end) noexcept;
    [[nodiscard]] bool                                 ZoomGpu(
        ProfileViewerGpuViewportKey _key,
        uint32                      _anchor_numerator,
        uint32                      _anchor_denominator,
        uint32                      _scale_numerator,
        uint32                      _scale_denominator
    ) noexcept;
    [[nodiscard]] bool PanGpu(ProfileViewerGpuViewportKey _key, int64 _delta) noexcept;
    [[nodiscard]] bool FocusGpu(
        ProfileViewerGpuViewportKey _key,
        uint64                      _begin,
        uint64                      _end,
        uint64                      _margin
    ) noexcept;

    [[nodiscard]] bool SelectCpu(
        uint64 _timeline_track_index,
        uint64 _source_scope_index,
        uint64 _begin,
        uint64 _end
    ) noexcept;
    [[nodiscard]] bool SelectGpu(
        ProfileViewerGpuViewportKey _key,
        uint64                      _timeline_track_index,
        uint64                      _source_scope_index,
        uint64                      _begin,
        uint64                      _end
    ) noexcept;
    [[nodiscard]] ProfileViewerSelection Selection() const noexcept;
    void                                 ClearSelection() noexcept;

    [[nodiscard]] std::size_t ActiveGpuViewportCount() const noexcept;

private:
    struct GpuViewportSlot {
        bool                        occupied{false};
        ProfileViewerGpuViewportKey key{};
        ProfileViewerViewport       viewport{};
    };

    [[nodiscard]] static bool IsValidGpuKey(ProfileViewerGpuViewportKey _key) noexcept;
    [[nodiscard]] static bool
                              FitViewport(ProfileViewerViewport& _viewport, uint64 _domain_end) noexcept;
    [[nodiscard]] static bool ZoomViewport(
        ProfileViewerViewport& _viewport,
        uint32                 _anchor_numerator,
        uint32                 _anchor_denominator,
        uint32                 _scale_numerator,
        uint32                 _scale_denominator
    ) noexcept;
    [[nodiscard]] static bool PanViewport(ProfileViewerViewport& _viewport, int64 _delta) noexcept;
    [[nodiscard]] static bool FocusViewport(
        ProfileViewerViewport& _viewport,
        uint64                 _begin,
        uint64                 _end,
        uint64                 _margin
    ) noexcept;
    [[nodiscard]] static bool ContainsInterval(
        const ProfileViewerViewport& _viewport,
        uint64                       _begin,
        uint64                       _end
    ) noexcept;

    [[nodiscard]] GpuViewportSlot*       FindGpuViewportSlot(ProfileViewerGpuViewportKey _key) noexcept;
    [[nodiscard]] const GpuViewportSlot* FindGpuViewportSlot(ProfileViewerGpuViewportKey _key) const noexcept;
    [[nodiscard]] GpuViewportSlot*       AcquireGpuViewportSlot(ProfileViewerGpuViewportKey _key) noexcept;
    void                                 ResetPublicationState() noexcept;

    uint64                                                         published_generation_{ProfileDump::kInvalidProfileDocumentGeneration};
    ProfileViewerViewport cpu_viewport_{};
    std::array<GpuViewportSlot, kProfileViewerGpuViewportCapacity> gpu_viewports_{};
    std::size_t                                                    active_gpu_viewport_count_{0};
    std::size_t                                                    next_gpu_eviction_{0};
    ProfileViewerSelection                                         selection_{};
};

} // namespace Moer
