#include "VulkanDeviceProperty.h"

void VulkanPhysicalDeviceProperties::Query(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

    if (_api_version >= VK_API_VERSION_1_1) {
        properties2.pNext = &core_1_1;
        core_1_1.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    }

    if (_api_version >= VK_API_VERSION_1_2) {
        core_1_1.pNext = &core_1_2;
        core_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    }

    if (_api_version >= VK_API_VERSION_1_3) {
        core_1_2.pNext = &core_1_3;
        core_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    }

    vkGetPhysicalDeviceProperties2(_gpu, &properties2);

    // Copy properties into old struct for convenience
    core_1_0 = properties2.properties;
}