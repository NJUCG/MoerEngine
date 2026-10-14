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

    void AttachFeatureChain(VkDeviceCreateInfo& _device_create_info, uint32_t _api_version);

private:
    VkPhysicalDeviceFeatures         core_1_0{};
    VkPhysicalDeviceVulkan11Features core_1_1{};
    VkPhysicalDeviceVulkan12Features core_1_2{};
    VkPhysicalDeviceVulkan13Features core_1_3{};

    friend class VulkanDevice;
};

bool operator>=(const VkPhysicalDeviceFeatures& _lhs, const VkPhysicalDeviceFeatures& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan11Features& _lhs, const VkPhysicalDeviceVulkan11Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan12Features& _lhs, const VkPhysicalDeviceVulkan12Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan13Features& _lhs, const VkPhysicalDeviceVulkan13Features& _rhs);

} // namespace Moer::Render

#endif //VULKAN_DEVICE_FEATURE_H
