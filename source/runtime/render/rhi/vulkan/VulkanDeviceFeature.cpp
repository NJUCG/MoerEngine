#include "VulkanDeviceFeature.h"
#include "VulkanPlatform.h"

namespace Moer::Render {

VulkanDeviceFeatures VulkanDeviceFeatures::BuildRequiredFeatures(uint32_t _api_version) {
    VulkanDeviceFeatures required_features;

    // 1.0 features
    required_features.core_1_0.samplerAnisotropy = VK_TRUE;
    required_features.core_1_0.sampleRateShading = VK_TRUE;
    required_features.core_1_0.depthClamp        = VK_TRUE;
    // 1.1 features
    if (_api_version >= VK_API_VERSION_1_1) {
        required_features.core_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    }

    // 1.2 features
    if (_api_version >= VK_API_VERSION_1_2) {

        required_features.core_1_2.timelineSemaphore   = VK_TRUE;
        required_features.core_1_2.bufferDeviceAddress = VK_TRUE;
        // MARK: need fallback to non-bindless if not supported
        // descriptor indexing features
        required_features.core_1_2.descriptorIndexing = VK_TRUE;
        // uniform
        required_features.core_1_2.shaderUniformBufferArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        // sampler
        required_features.core_1_2.shaderSampledImageArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        // image
        required_features.core_1_2.shaderStorageImageArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
        // buffer
        required_features.core_1_2.shaderStorageBufferArrayNonUniformIndexing    = VK_TRUE;
        required_features.core_1_2.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        required_features.core_1_2.drawIndirectCount                             = VK_TRUE;
    }

    // 1.3 features
    if (_api_version >= VK_API_VERSION_1_3) {

        required_features.core_1_3.synchronization2 = VK_TRUE;
        required_features.core_1_3.dynamicRendering = VK_TRUE;
    }

    // Apply platform restrictions
    VulkanPlatform::RestrictEnabledPhysicalDeviceFeatures(required_features.core_1_0);

    return required_features;
}

VulkanDeviceFeatures VulkanDeviceFeatures::QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32_t _api_version) {
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

void VulkanDeviceFeatures::AttachFeatureChain(VkDeviceCreateInfo& _device_create_info, uint32_t _api_version) {
    if (_api_version >= VK_API_VERSION_1_3) {
        core_1_3.pNext = (void*)_device_create_info.pNext;
        core_1_2.pNext = &core_1_3;
        core_1_1.pNext = &core_1_2;
    } else if (_api_version == VK_API_VERSION_1_2) {
        core_1_2.pNext = (void*)_device_create_info.pNext;
        core_1_1.pNext = &core_1_2;
    } else {
        core_1_1.pNext = (void*)_device_create_info.pNext;
    }
}

} // namespace Moer::Render
