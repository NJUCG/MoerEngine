#pragma once

#include "profile_consumer/ProfileDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Moer {

inline constexpr std::size_t kProfileViewerGpuViewportCapacity = 256;

// Maps a bounded rational position onto [begin, end] without converting the
// uint64_t domain to floating point. Invalid fractions map to begin.
[[nodiscard]] std::uint64_t ProfileViewerMapFraction(
    std::uint64_t _begin,
    std::uint64_t _end,
    std::uint32_t _numerator,
    std::uint32_t _denominator
) noexcept;

struct ProfileViewerViewport {
    bool          valid{false};
    std::uint64_t domain_end{0};
    std::uint64_t view_begin{0};
    std::uint64_t view_end{0};

    friend bool operator==(const ProfileViewerViewport&, const ProfileViewerViewport&) = default;
};

struct ProfileViewerGpuViewportKey {
    std::uint32_t axis_index{ProfileDump::kInvalidTimelineAxis};
    std::uint64_t frame_id{0};

    friend bool operator==(const ProfileViewerGpuViewportKey&, const ProfileViewerGpuViewportKey&) = default;
};

enum class EProfileViewerSelectionKind : std::uint8_t {
    None = 0,
    CpuScope,
    GpuScope,
};

struct ProfileViewerSelection {
    EProfileViewerSelectionKind kind{EProfileViewerSelectionKind::None};
    std::uint64_t               published_generation{ProfileDump::kInvalidProfileDocumentGeneration};
    std::uint64_t               source_scope_index{ProfileDump::kInvalidSessionIndex};
    std::uint64_t               timeline_track_index{ProfileDump::kInvalidSessionIndex};
    std::uint32_t               axis_index{ProfileDump::kInvalidTimelineAxis};
    std::uint64_t               frame_id{0};
    std::uint64_t               begin{0};
    std::uint64_t               end{0};

    [[nodiscard]] bool Valid() const noexcept;

    friend bool operator==(const ProfileViewerSelection&, const ProfileViewerSelection&) = default;
};

enum class EProfileViewerPublicationUpdate : std::uint8_t {
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
    ObserveSnapshot(const ProfileDump::ProfileDocumentLoaderSnapshot& _snapshot) noexcept;

    [[nodiscard]] std::uint64_t PublishedGeneration() const noexcept;

    [[nodiscard]] ProfileViewerViewport CpuViewport() const noexcept;
    [[nodiscard]] bool                  FitCpu(std::uint64_t _domain_end) noexcept;
    [[nodiscard]] bool                  ZoomCpu(
                         std::uint32_t _anchor_numerator,
                         std::uint32_t _anchor_denominator,
                         std::uint32_t _scale_numerator,
                         std::uint32_t _scale_denominator
                     ) noexcept;
    [[nodiscard]] bool PanCpu(std::int64_t _delta) noexcept;
    [[nodiscard]] bool FocusCpu(std::uint64_t _begin, std::uint64_t _end, std::uint64_t _margin) noexcept;

    [[nodiscard]] std::optional<ProfileViewerViewport> FindGpuViewport(ProfileViewerGpuViewportKey _key
    ) const noexcept;
    [[nodiscard]] bool FitGpu(ProfileViewerGpuViewportKey _key, std::uint64_t _domain_end) noexcept;
    [[nodiscard]] bool ZoomGpu(
        ProfileViewerGpuViewportKey _key,
        std::uint32_t               _anchor_numerator,
        std::uint32_t               _anchor_denominator,
        std::uint32_t               _scale_numerator,
        std::uint32_t               _scale_denominator
    ) noexcept;
    [[nodiscard]] bool PanGpu(ProfileViewerGpuViewportKey _key, std::int64_t _delta) noexcept;
    [[nodiscard]] bool FocusGpu(
        ProfileViewerGpuViewportKey _key,
        std::uint64_t               _begin,
        std::uint64_t               _end,
        std::uint64_t               _margin
    ) noexcept;

    [[nodiscard]] bool SelectCpu(
        std::uint64_t _timeline_track_index,
        std::uint64_t _source_scope_index,
        std::uint64_t _begin,
        std::uint64_t _end
    ) noexcept;
    [[nodiscard]] bool SelectGpu(
        ProfileViewerGpuViewportKey _key,
        std::uint64_t               _timeline_track_index,
        std::uint64_t               _source_scope_index,
        std::uint64_t               _begin,
        std::uint64_t               _end
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
    FitViewport(ProfileViewerViewport& _viewport, std::uint64_t _domain_end) noexcept;
    [[nodiscard]] static bool ZoomViewport(
        ProfileViewerViewport& _viewport,
        std::uint32_t          _anchor_numerator,
        std::uint32_t          _anchor_denominator,
        std::uint32_t          _scale_numerator,
        std::uint32_t          _scale_denominator
    ) noexcept;
    [[nodiscard]] static bool PanViewport(ProfileViewerViewport& _viewport, std::int64_t _delta) noexcept;
    [[nodiscard]] static bool FocusViewport(
        ProfileViewerViewport& _viewport,
        std::uint64_t          _begin,
        std::uint64_t          _end,
        std::uint64_t          _margin
    ) noexcept;
    [[nodiscard]] static bool ContainsInterval(
        const ProfileViewerViewport& _viewport,
        std::uint64_t                _begin,
        std::uint64_t                _end
    ) noexcept;

    [[nodiscard]] GpuViewportSlot*       FindGpuViewportSlot(ProfileViewerGpuViewportKey _key) noexcept;
    [[nodiscard]] const GpuViewportSlot* FindGpuViewportSlot(ProfileViewerGpuViewportKey _key) const noexcept;
    [[nodiscard]] GpuViewportSlot*       AcquireGpuViewportSlot(ProfileViewerGpuViewportKey _key) noexcept;
    void                                 ResetPublicationState() noexcept;

    std::uint64_t         published_generation_{ProfileDump::kInvalidProfileDocumentGeneration};
    ProfileViewerViewport cpu_viewport_{};
    std::array<GpuViewportSlot, kProfileViewerGpuViewportCapacity> gpu_viewports_{};
    std::size_t                                                    active_gpu_viewport_count_{0};
    std::size_t                                                    next_gpu_eviction_{0};
    ProfileViewerSelection                                         selection_{};
};

} // namespace Moer
