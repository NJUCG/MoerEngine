#pragma once

#include "PixelFormat.h"
#include "VulkanCommon.h"
#include "rhi/RHICommon.h"

namespace Moer::Render {

enum class VulkanImageSampling : uint8 {
    Unknown = 0,
    Sampled = 1,
    Storage = 2
};

struct VulkanArgumentInfo {
    VkPipelineStageFlags2 stages         = VK_PIPELINE_STAGE_2_NONE;
    EShaderResourceType   resource_type  = SRT_INVALID;
    VulkanImageSampling   image_sampling = VulkanImageSampling::Unknown;
    EPixelFormat          texel_format   = PF_UNDEFINED;

    bool IsWritable() const {
        return resource_type == SRT_UAV;
    }

    ERHIAccessFlags GetAccess() const {
        switch (resource_type) {
            case SRT_UAV:
                return ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE;
            case SRT_CBV:
            case SRT_SRV:
                return ERHIAccessFlags::SHADER_READ;
            default:
                return ERHIAccessFlags::UNDEFINED;
        }
    }
};

static_assert(sizeof(VulkanArgumentInfo) == 16);

} // namespace Moer::Render
