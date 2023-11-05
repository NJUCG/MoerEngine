//
// Created by 74535 on 2023/10/20.
//

#include "VulkanDeviceFeature.h"

#include "VulkanPlatform.h"

void VulkanPhysicalDeviceFeatures::Query(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VkPhysicalDeviceFeatures2 gpu_features_2;
    gpu_features_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    gpu_features_2.pNext = &core_1_1;
    core_1_1.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;

    if (_api_version >= VK_API_VERSION_1_2) {
        core_1_1.pNext = &core_1_2;
        core_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    }

    if (_api_version >= VK_API_VERSION_1_3) {
        core_1_2.pNext = &core_1_3;
        core_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    }

    vkGetPhysicalDeviceFeatures2(_gpu, &gpu_features_2);

    // Copy features into old struct for convenience
    core_1_0 = gpu_features_2.features;
}

VulkanPhysicalDeviceFeatures VulkanDeviceFeature::GetMESupportedDeviceFeatures(uint32_t _api_version) {
    VulkanPhysicalDeviceFeatures enabled_features;

    // 1.0 features
    enabled_features.core_1_0.samplerAnisotropy = VK_TRUE;
    enabled_features.core_1_0.sampleRateShading = VK_TRUE;

    // 1.1 features
    if (_api_version >= VK_API_VERSION_1_1) {
        enabled_features.core_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    }

    // 1.2 features
    if (_api_version >= VK_API_VERSION_1_2) {
        enabled_features.core_1_1.pNext = &enabled_features.core_1_2;
        enabled_features.core_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    }

    // 1.3 features
    if (_api_version >= VK_API_VERSION_1_3) {
        enabled_features.core_1_2.pNext = &enabled_features.core_1_3;
        enabled_features.core_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        enabled_features.core_1_3.dynamicRendering = VK_TRUE;
    }

    // Apply platform restrictions
    VulkanPlatform::RestrictEnabledPhysicalDeviceFeatures(&enabled_features);

    // Custom features

    return enabled_features;
}
