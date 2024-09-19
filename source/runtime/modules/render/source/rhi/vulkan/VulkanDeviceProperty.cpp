#include "VulkanDeviceProperty.h"

VulkanCoreDeviceProperties VulkanCoreDeviceProperties::GetGpuCoreProperties(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VulkanCoreDeviceProperties props;

    VkPhysicalDeviceProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

    if (_api_version >= VK_API_VERSION_1_1) {
        properties2.pNext    = &props.core_1_1;
        props.core_1_1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    }

    if (_api_version >= VK_API_VERSION_1_2) {
        props.core_1_1.pNext = &props.core_1_2;
        props.core_1_2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    }

    if (_api_version >= VK_API_VERSION_1_3) {
        props.core_1_2.pNext = &props.core_1_3;
        props.core_1_3.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    }

    vkGetPhysicalDeviceProperties2(_gpu, &properties2);

    // Copy properties into old struct for convenience
    props.core_1_0 = properties2.properties;

    return props;
}