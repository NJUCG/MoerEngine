#include <volk.h>
#include "VulkanDeviceFeature.h"
#include "VulkanPlatform.h"

VulkanDeviceFeatures VulkanDeviceFeatures::GetMERequiredFeatures(uint32_t _api_version) {
    VulkanDeviceFeatures enabled_features;

    // 1.0 features
    enabled_features.core_1_0.samplerAnisotropy = VK_TRUE;
    enabled_features.core_1_0.sampleRateShading = VK_TRUE;
    enabled_features.core_1_0.depthClamp        = VK_TRUE;
    // 1.1 features
    if (_api_version >= VK_API_VERSION_1_1) {
        enabled_features.core_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    }

    // 1.2 features
    if (_api_version >= VK_API_VERSION_1_2) {

        enabled_features.core_1_2.timelineSemaphore   = VK_TRUE;
        enabled_features.core_1_2.bufferDeviceAddress = VK_TRUE;
        // MARK: need fallback to non-bindless if not supported
        // descriptor indexing features
        enabled_features.core_1_2.descriptorIndexing = VK_TRUE;
        // uniform
        enabled_features.core_1_2.shaderUniformBufferArrayNonUniformIndexing    = VK_TRUE;
        enabled_features.core_1_2.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        // sampler
        enabled_features.core_1_2.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
        enabled_features.core_1_2.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        // image
        enabled_features.core_1_2.shaderStorageImageArrayNonUniformIndexing    = VK_TRUE;
        enabled_features.core_1_2.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        // buffer
        enabled_features.core_1_2.shaderStorageBufferArrayNonUniformIndexing    = VK_TRUE;
        enabled_features.core_1_2.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        enabled_features.core_1_2.drawIndirectCount                             = VK_TRUE;
    }

    // 1.3 features
    if (_api_version >= VK_API_VERSION_1_3) {

        enabled_features.core_1_3.synchronization2 = VK_TRUE;
        enabled_features.core_1_3.dynamicRendering = VK_TRUE;
    }

    // Apply platform restrictions
    VulkanPlatform::RestrictEnabledPhysicalDeviceFeatures(&enabled_features);

    return enabled_features;
}

VulkanDeviceFeatures VulkanDeviceFeatures::GetGpuFeatures(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VulkanDeviceFeatures gpu_features;

    VkPhysicalDeviceFeatures2 gpu_features_2{};
    gpu_features_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    if (_api_version >= VK_API_VERSION_1_1) {
        gpu_features_2.pNext        = &gpu_features.core_1_1;
        gpu_features.core_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    }

    if (_api_version >= VK_API_VERSION_1_2) {
        gpu_features.core_1_1.pNext = &gpu_features.core_1_2;
        gpu_features.core_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    }

    if (_api_version >= VK_API_VERSION_1_3) {
        gpu_features.core_1_2.pNext = &gpu_features.core_1_3;
        gpu_features.core_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    }

    vkGetPhysicalDeviceFeatures2(_gpu, &gpu_features_2);

    // Copy features into old struct for convenience
    gpu_features.core_1_0 = gpu_features_2.features;

    return gpu_features;
}

bool VulkanDeviceFeatures::Contains(const VulkanDeviceFeatures& _other) const {
    return core_1_0 >= _other.core_1_0 && core_1_1 >= _other.core_1_1 && core_1_2 >= _other.core_1_2 && core_1_3 >= _other.core_1_3;
}

void VulkanDeviceFeatures::PreCreateDevice(VkDeviceCreateInfo& _device_create_info, uint32_t _api_version) {
    if (_api_version >= VK_API_VERSION_1_3) {
        core_1_3.pNext = (void*)_device_create_info.pNext;
    } else if (_api_version == VK_API_VERSION_1_2) {
        core_1_2.pNext = (void*)_device_create_info.pNext;
    } else {
        core_1_1.pNext = (void*)_device_create_info.pNext;
    }
}

bool operator>=(const VkPhysicalDeviceFeatures& _lhs, const VkPhysicalDeviceFeatures& _rhs) {
    auto* lp = (VkBool32*)&_lhs;
    auto* rp = (VkBool32*)&_rhs;
    for (size_t i = 0; i < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++i) {
        if (lp[i] < rp[i]) {
            return false;
        }
    }
    return true;
}

bool operator>=(const VkPhysicalDeviceVulkan11Features& _lhs, const VkPhysicalDeviceVulkan11Features& _rhs) {
    auto* lp = (VkBool32*)&_lhs;
    auto* rp = (VkBool32*)&_rhs;
    for (size_t i = 4; i < sizeof(VkPhysicalDeviceVulkan11Features) / sizeof(VkBool32); ++i) {
        if (lp[i] < rp[i]) {
            return false;
        }
    }
    return true;
}

bool operator>=(const VkPhysicalDeviceVulkan12Features& _lhs, const VkPhysicalDeviceVulkan12Features& _rhs) {
    auto* lp = (VkBool32*)&_lhs;
    auto* rp = (VkBool32*)&_rhs;
    for (size_t i = 4; i < sizeof(VkPhysicalDeviceVulkan12Features) / sizeof(VkBool32); ++i) {
        if (lp[i] < rp[i]) {
            return false;
        }
    }
    return true;
}

bool operator>=(const VkPhysicalDeviceVulkan13Features& _lhs, const VkPhysicalDeviceVulkan13Features& _rhs) {
    auto* lp = (VkBool32*)&_lhs;
    auto* rp = (VkBool32*)&_rhs;
    for (size_t i = 4; i < sizeof(VkPhysicalDeviceVulkan13Features) / sizeof(VkBool32); ++i) {
        if (lp[i] < rp[i]) {
            return false;
        }
    }
    return true;
}
