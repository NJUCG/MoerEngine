#include "VulkanDeviceFeature.h"
#include "VulkanDeviceFeatureChain.h"

#include <algorithm>

namespace Moer::Render {

VulkanDeviceFeatures VulkanDeviceFeatures::BuildRequiredFeatures(uint32_t _api_version) {
    VulkanDeviceFeatures required_features;

    // 1.0 features
    required_features.core_1_0.samplerAnisotropy = VK_TRUE;
    required_features.core_1_0.sampleRateShading = VK_TRUE;
    required_features.core_1_0.depthClamp        = VK_TRUE;
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

    return required_features;
}

VulkanDeviceFeatures
VulkanDeviceFeatures::QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(_gpu, &properties);
    const uint32_t           query_api_version = std::min(_api_version, properties.apiVersion);
    VulkanDeviceFeatureChain chain({}, query_api_version);
    if (query_api_version >= VK_API_VERSION_1_1) {
        vkGetPhysicalDeviceFeatures2(_gpu, &chain.GetFeatures2());
    } else {
        vkGetPhysicalDeviceFeatures(_gpu, &chain.GetFeatures2().features);
    }
    return chain.CaptureFeatures();
}

} // namespace Moer::Render
