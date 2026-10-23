#include "VulkanDeviceProperty.h"

#include <algorithm>

namespace Moer::Render {
namespace {

VkPhysicalDeviceVulkan11Properties QueryVulkan11Properties(VkPhysicalDevice _gpu) {
    // VkPhysicalDeviceVulkan11Properties was introduced in Vulkan 1.2.
    // Vulkan 1.1 queries use the individual structures, then normalize the result.
    VkPhysicalDeviceMaintenance3Properties maintenance3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES
    };
    VkPhysicalDeviceProtectedMemoryProperties protected_memory{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES, &maintenance3
    };
    VkPhysicalDeviceMultiviewProperties multiview{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, &protected_memory
    };
    VkPhysicalDevicePointClippingProperties point_clipping{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES, &multiview
    };
    VkPhysicalDeviceSubgroupProperties subgroup{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &point_clipping
    };
    VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &subgroup};
    VkPhysicalDeviceProperties2  properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};

    vkGetPhysicalDeviceProperties2(_gpu, &properties2);

    VkPhysicalDeviceVulkan11Properties properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    std::copy_n(identity.deviceUUID, VK_UUID_SIZE, properties.deviceUUID);
    std::copy_n(identity.driverUUID, VK_UUID_SIZE, properties.driverUUID);
    std::copy_n(identity.deviceLUID, VK_LUID_SIZE, properties.deviceLUID);
    properties.deviceNodeMask                    = identity.deviceNodeMask;
    properties.deviceLUIDValid                   = identity.deviceLUIDValid;
    properties.subgroupSize                      = subgroup.subgroupSize;
    properties.subgroupSupportedStages           = subgroup.supportedStages;
    properties.subgroupSupportedOperations       = subgroup.supportedOperations;
    properties.subgroupQuadOperationsInAllStages = subgroup.quadOperationsInAllStages;
    properties.pointClippingBehavior             = point_clipping.pointClippingBehavior;
    properties.maxMultiviewViewCount             = multiview.maxMultiviewViewCount;
    properties.maxMultiviewInstanceIndex         = multiview.maxMultiviewInstanceIndex;
    properties.protectedNoFault                  = protected_memory.protectedNoFault;
    properties.maxPerSetDescriptors              = maintenance3.maxPerSetDescriptors;
    properties.maxMemoryAllocationSize           = maintenance3.maxMemoryAllocationSize;
    return properties;
}

} // namespace

VulkanCoreDeviceProperties
VulkanCoreDeviceProperties::QueryGpuProperties(VkPhysicalDevice _gpu, uint32_t _api_version) {
    VulkanCoreDeviceProperties properties;
    vkGetPhysicalDeviceProperties(_gpu, &properties.core_1_0);
    const uint32_t query_api_version = std::min(_api_version, properties.core_1_0.apiVersion);

    if (query_api_version < VK_API_VERSION_1_1) {
        return properties;
    }
    if (query_api_version < VK_API_VERSION_1_2) {
        properties.core_1_1 = QueryVulkan11Properties(_gpu);
        return properties;
    }

    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &properties.core_1_1
    };
    properties.core_1_1.pNext = &properties.core_1_2;
    if (query_api_version >= VK_API_VERSION_1_3) {
        properties.core_1_2.pNext = &properties.core_1_3;
    }
    vkGetPhysicalDeviceProperties2(_gpu, &properties2);

    properties.core_1_1.pNext = nullptr;
    properties.core_1_2.pNext = nullptr;
    return properties;
}

} // namespace Moer::Render
