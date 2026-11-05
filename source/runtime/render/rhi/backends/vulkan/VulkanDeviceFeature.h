#pragma once

#include "VulkanCommon.h"

namespace Moer::Render {

// Value snapshot. Native query/create chains are local to their Vulkan calls.
class VulkanDeviceFeatures {
public:
    static VulkanDeviceFeatures QuerySupportedFeatures(VkPhysicalDevice gpu);
    bool SupportsRequiredFeatures() const;

    const VkPhysicalDeviceFeatures& GetCore10Features() const { return core_1_0; }
    const VkPhysicalDeviceVulkan11Features& GetCore11Features() const { return core_1_1; }
    const VkPhysicalDeviceVulkan12Features& GetCore12Features() const { return core_1_2; }
    const VkPhysicalDeviceVulkan13Features& GetCore13Features() const { return core_1_3; }

private:
    VkPhysicalDeviceFeatures         core_1_0{};
    VkPhysicalDeviceVulkan11Features core_1_1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features core_1_2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features core_1_3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
};

} // namespace Moer::Render
