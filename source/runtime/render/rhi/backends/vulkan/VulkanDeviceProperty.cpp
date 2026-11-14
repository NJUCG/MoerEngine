#include "VulkanDeviceProperty.h"

#include <stdexcept>

namespace Moer::Render {

VulkanCoreDeviceProperties VulkanCoreDeviceProperties::QueryGpuProperties(VkPhysicalDevice gpu) {
    VulkanCoreDeviceProperties properties;
    vkGetPhysicalDeviceProperties(gpu, &properties.core_1_0);
    if (properties.core_1_0.apiVersion < k_vulkan_api_version) {
        throw std::runtime_error("Vulkan 1.3 or newer is required");
    }

    properties.core_1_1.pNext = &properties.core_1_2;
    properties.core_1_2.pNext = &properties.core_1_3;
    VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &properties.core_1_1};
    vkGetPhysicalDeviceProperties2(gpu, &query);
    properties.core_1_0 = query.properties;
    properties.core_1_1.pNext = nullptr;
    properties.core_1_2.pNext = nullptr;
    return properties;
}

} // namespace Moer::Render
