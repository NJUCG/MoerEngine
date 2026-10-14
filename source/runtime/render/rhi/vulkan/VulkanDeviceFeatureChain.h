#pragma once

#include "VulkanDeviceFeature.h"

namespace Moer::Render {

// Owns the native query/create chain for one Vulkan call. Internal pointers must
// keep referring to this object, so the chain cannot be copied or moved.
class VulkanDeviceFeatureChain {
public:
    VulkanDeviceFeatureChain(const VulkanDeviceFeatures& _features, uint32_t _api_version);

    VulkanDeviceFeatureChain(const VulkanDeviceFeatureChain&)            = delete;
    VulkanDeviceFeatureChain& operator=(const VulkanDeviceFeatureChain&) = delete;
    VulkanDeviceFeatureChain(VulkanDeviceFeatureChain&&)                 = delete;
    VulkanDeviceFeatureChain& operator=(VulkanDeviceFeatureChain&&)      = delete;

    VkPhysicalDeviceFeatures2& GetFeatures2() {
        return features2;
    }

    VulkanDeviceFeatures CaptureFeatures() const;
    void                 AttachToDeviceCreateInfo(VkDeviceCreateInfo& _device_create_info);

private:
    void InitializeVulkan11Chain(const VkPhysicalDeviceVulkan11Features& _features);
    void CaptureVulkan11Features(VkPhysicalDeviceVulkan11Features& _features) const;

    uint32_t                         api_version;
    VkPhysicalDeviceFeatures2        features2{};
    VkPhysicalDeviceVulkan11Features core_1_1{};
    VkPhysicalDeviceVulkan12Features core_1_2{};
    VkPhysicalDeviceVulkan13Features core_1_3{};

    // Vulkan11Features was introduced in 1.2. A 1.1 device uses the individual
    // feature structures that were available in that API version.
    VkPhysicalDevice16BitStorageFeatures storage_16bit{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES
    };
    VkPhysicalDeviceMultiviewFeatures        multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceVariablePointersFeatures variable_pointers{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES
    };
    VkPhysicalDeviceProtectedMemoryFeatures protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES
    };
    VkPhysicalDeviceShaderDrawParametersFeatures shader_draw_parameters{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
    };
};

} // namespace Moer::Render
