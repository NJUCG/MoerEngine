#include "VulkanDeviceFeature.h"

#include <stdexcept>

namespace Moer::Render {

VulkanDeviceFeatures VulkanDeviceFeatures::QuerySupportedFeatures(VkPhysicalDevice gpu) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu, &properties);
    if (properties.apiVersion < k_vulkan_api_version) {
        throw std::runtime_error("Vulkan 1.3 or newer is required");
    }

    VulkanDeviceFeatures features;
    features.core_1_1.pNext = &features.core_1_2;
    features.core_1_2.pNext = &features.core_1_3;
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features.core_1_1};
    vkGetPhysicalDeviceFeatures2(gpu, &query);
    features.core_1_0 = query.features;
    features.core_1_1.pNext = nullptr;
    features.core_1_2.pNext = nullptr;
    return features;
}

bool VulkanDeviceFeatures::SupportsRequiredFeatures() const {
    // These are the fixed requirements of the renderer's bindless and recording paths.
    return core_1_0.samplerAnisotropy && core_1_0.sampleRateShading && core_1_0.depthClamp &&
           core_1_0.shaderInt64 && core_1_2.timelineSemaphore && core_1_2.bufferDeviceAddress &&
           core_1_2.descriptorIndexing && core_1_2.shaderUniformBufferArrayNonUniformIndexing &&
           core_1_2.descriptorBindingUniformBufferUpdateAfterBind &&
           core_1_2.shaderSampledImageArrayNonUniformIndexing &&
           core_1_2.descriptorBindingSampledImageUpdateAfterBind &&
           core_1_2.shaderStorageImageArrayNonUniformIndexing &&
           core_1_2.descriptorBindingStorageImageUpdateAfterBind &&
           core_1_2.shaderStorageBufferArrayNonUniformIndexing &&
           core_1_2.descriptorBindingStorageBufferUpdateAfterBind && core_1_2.drawIndirectCount &&
           core_1_3.synchronization2 && core_1_3.dynamicRendering;
}

} // namespace Moer::Render
