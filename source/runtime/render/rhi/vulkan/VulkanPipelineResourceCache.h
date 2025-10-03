#ifndef VULKAN_PIPELINE_STATE_CACHE_H
#define VULKAN_PIPELINE_STATE_CACHE_H

#include "VulkanDescriptor.h"
#include "rhi/RHIResource.h"
namespace Moer::Render {
    class VulkanDescriptorSetsLayout;
    class VulkanDevice;

    struct PushConstantInfo {
        VkShaderStageFlags   flags;
        uint32_t             size;
        uint32_t             byte_offset_in_raw_data;
        Moer::Array<uint8_t> raw_data;
    };
}// namespace Moer::Render
#endif// VULKAN_PIPELINE_STATE_CACHE_H
