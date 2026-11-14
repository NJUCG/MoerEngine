#pragma once

#include "misc/Types.h"
#include <bit>

namespace Moer::Render::Multiview {

[[nodiscard]] constexpr uint32 RequiredViewCount(uint32 view_mask) noexcept {
    return std::bit_width(view_mask);
}

[[nodiscard]] constexpr bool SupportsViewCount(
    bool   feature_enabled,
    uint32 max_view_count,
    uint32 requested_view_count
) noexcept {
    return requested_view_count != 0u && feature_enabled &&
           max_view_count >= requested_view_count;
}

[[nodiscard]] constexpr bool IsAttachmentRangeValid(
    uint32 base_layer,
    uint32 layer_count,
    uint32 texture_layer_count
) noexcept {
    return layer_count != 0u && base_layer <= texture_layer_count &&
           layer_count <= texture_layer_count - base_layer;
}

[[nodiscard]] constexpr bool AttachmentCoversViewMask(
    uint32 layer_count,
    uint32 view_mask
) noexcept {
    return layer_count >= RequiredViewCount(view_mask);
}

[[nodiscard]] constexpr bool PipelineMatchesRenderPass(
    uint32 pipeline_view_mask,
    uint32 render_pass_view_mask
) noexcept {
    return pipeline_view_mask == render_pass_view_mask;
}

} // namespace Moer::Render::Multiview
