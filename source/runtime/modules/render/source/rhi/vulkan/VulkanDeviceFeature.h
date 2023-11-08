//
// Created by 74535 on 2023/10/20.
//

#ifndef VULKAN_DEVICE_FEATURE_H
#define VULKAN_DEVICE_FEATURE_H

#include <vulkan/vulkan.h>

class VulkanPhysicalDeviceFeatures {
public:
    VulkanPhysicalDeviceFeatures() : core_1_0(), core_1_1(), core_1_2(), core_1_3() {}
    VulkanPhysicalDeviceFeatures(const VulkanPhysicalDeviceFeatures& _other) = default;

    void Query(VkPhysicalDevice _gpu, uint32_t _api_version);

    bool Contains(const VulkanPhysicalDeviceFeatures& _other) const;

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

class VulkanDeviceFeature {
public:
    static VulkanPhysicalDeviceFeatures GetMESupportedDeviceFeatures(uint32_t _api_version);
};

bool operator>=(const VkPhysicalDeviceFeatures& _lhs, const VkPhysicalDeviceFeatures& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan11Features& _lhs, const VkPhysicalDeviceVulkan11Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan12Features& _lhs, const VkPhysicalDeviceVulkan12Features& _rhs);
bool operator>=(const VkPhysicalDeviceVulkan13Features& _lhs, const VkPhysicalDeviceVulkan13Features& _rhs);

#endif//VULKAN_DEVICE_FEATURE_H
