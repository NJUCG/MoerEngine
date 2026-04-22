#ifndef VULKAN_DEVICE_FEATURE_H
#define VULKAN_DEVICE_FEATURE_H

#include "VulkanPlatform.h"

namespace Moer::Render {

class VulkanDeviceFeatures {
public:
    VulkanDeviceFeatures() : core_1_0(), core_1_1(), core_1_2(), core_1_3() {}
    VulkanDeviceFeatures(const VulkanDeviceFeatures& _other) = default;

    static VulkanDeviceFeatures GetGpuFeatures(VkPhysicalDevice _gpu, uint32_t _api_version);

    static VulkanDeviceFeatures GetMERequiredFeatures(uint32_t _api_version);

    bool Contains(const VulkanDeviceFeatures& _other) const;

    inline const VkPhysicalDeviceVulkan11Features& GetCore11Features() const {
        return core_1_1;
    }

    inline const VkPhysicalDeviceVulkan12Features& GetCore12Features() const {
        return core_1_2;
    }

    void PreCreateDevice(VkDeviceCreateInfo& _device_create_info, uint32_t _api_version);

    VkPhysicalDeviceFeatures         core_1_0;
    VkPhysicalDeviceVulkan11Features core_1_1;

private:
    // Anything above Core 1.1 cannot be assumed, they should only be used by the device at init time
    VkPhysicalDeviceVulkan12Features core_1_2;
    VkPhysicalDeviceVulkan13Features core_1_3;

    friend class VulkanDevice;
    friend class VulkanDeviceFeature;
};

bool operator>=(const VkPhysicalDeviceFeatures& _lhs, const VkPhysicalDeviceFeatures& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan11Features& _lhs, const VkPhysicalDeviceVulkan11Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan12Features& _lhs, const VkPhysicalDeviceVulkan12Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan13Features& _lhs, const VkPhysicalDeviceVulkan13Features& _rhs);

} // namespace Moer::Render

#endif //VULKAN_DEVICE_FEATURE_H
