#ifndef VULKAN_DEVICE_FEATURE_H
#define VULKAN_DEVICE_FEATURE_H

#include "VulkanCommon.h"

namespace Moer::Render {

class VulkanDeviceFeatures {
public:
    static VulkanDeviceFeatures QuerySupportedFeatures(VkPhysicalDevice _gpu, uint32_t _api_version);

    static VulkanDeviceFeatures BuildRequiredFeatures(uint32_t _api_version);

    bool SupportsAll(const VulkanDeviceFeatures& _required_features) const;

    const VkPhysicalDeviceFeatures& GetCore10Features() const {
        return core_1_0;
    }

    const VkPhysicalDeviceVulkan11Features& GetCore11Features() const {
        return core_1_1;
    }

    const VkPhysicalDeviceVulkan12Features& GetCore12Features() const {
        return core_1_2;
    }

    const VkPhysicalDeviceVulkan13Features& GetCore13Features() const {
        return core_1_3;
    }

private:
    VkPhysicalDeviceFeatures         core_1_0{};
    VkPhysicalDeviceVulkan11Features core_1_1{};
    VkPhysicalDeviceVulkan12Features core_1_2{};
    VkPhysicalDeviceVulkan13Features core_1_3{};

    friend class VulkanDeviceFeatureChain;
};

} // namespace Moer::Render

#endif //VULKAN_DEVICE_FEATURE_H
