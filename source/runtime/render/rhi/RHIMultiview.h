#pragma once

#include <bit>
#include <cstdint>

namespace Moer::Render::Multiview {

[[nodiscard]] constexpr uint32_t RequiredViewCount(uint32_t view_mask) noexcept {
    return std::bit_width(view_mask);
}

[[nodiscard]] constexpr bool SupportsViewCount(
    bool     feature_enabled,
    uint32_t max_view_count,
    uint32_t requested_view_count
) noexcept {
    return requested_view_count != 0u && feature_enabled &&
           max_view_count >= requested_view_count;
}

[[nodiscard]] constexpr bool IsAttachmentRangeValid(
    uint32_t base_layer,
    uint32_t layer_count,
    uint32_t texture_layer_count
) noexcept {
    return layer_count != 0u && base_layer <= texture_layer_count &&
           layer_count <= texture_layer_count - base_layer;
}

[[nodiscard]] constexpr bool AttachmentCoversViewMask(
    uint32_t layer_count,
    uint32_t view_mask
) noexcept {
    return layer_count >= RequiredViewCount(view_mask);
}

[[nodiscard]] constexpr bool PipelineMatchesRenderPass(
    uint32_t pipeline_view_mask,
    uint32_t render_pass_view_mask
) noexcept {
    return pipeline_view_mask == render_pass_view_mask;
}

} // namespace Moer::Render::Multiview
