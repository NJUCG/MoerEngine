#ifndef VULKAN_DEVICE_PROPERTY_H
#define VULKAN_DEVICE_PROPERTY_H

#include <vulkan/vulkan_core.h>

class VulkanPhysicalDeviceProperties {
public:
    VulkanPhysicalDeviceProperties() : core_1_0(), core_1_1(), core_1_2(), core_1_3() {}
    VulkanPhysicalDeviceProperties(const VulkanPhysicalDeviceProperties& _other) = default;

    void Query(VkPhysicalDevice _gpu, uint32_t _api_version);

    // core
    VkPhysicalDeviceProperties         core_1_0;
    VkPhysicalDeviceVulkan11Properties core_1_1;
    VkPhysicalDeviceVulkan12Properties core_1_2;
    VkPhysicalDeviceVulkan13Properties core_1_3;
};

class VulkanOptionalDeviceProperties {
public:
    // descriptor buffer
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_properties;

    // ray tracing
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR    ray_tracing_pipeline_properties;
};

#endif