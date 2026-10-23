#pragma once

#include "VulkanCommon.h"
#include "rhi/RHICommon.h"

namespace Moer::Render {

struct VulkanBufferAccessState {
    VkAccessFlags2        access{VK_ACCESS_2_NONE};
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
};

struct VulkanTextureAccessState {
    VkAccessFlags2        access{VK_ACCESS_2_NONE};
    VkImageLayout         layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
};

// Pure conversion of legacy RHI declarations. Invalid state/pass combinations
// throw; these functions neither track resources nor emit barriers.
RENDER_API VulkanBufferAccessState
ResolveVulkanBufferState(EBufferState _state, EPassType _pass_type, bool _is_write);

RENDER_API VulkanTextureAccessState ResolveVulkanTextureState(
    ETextureState       _state,
    EPassType           _pass_type,
    bool                _is_write,
    ETextureAspectFlags _aspects
);

} // namespace Moer::Render
