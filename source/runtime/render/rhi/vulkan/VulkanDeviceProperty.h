#ifndef VULKAN_DEVICE_PROPERTY_H
#define VULKAN_DEVICE_PROPERTY_H

#include "VulkanCommon.h"
#include "misc/STL.h"

namespace Moer::Render {

// A value snapshot: QueryGpuProperties detaches all temporary pNext links before returning.
class VulkanCoreDeviceProperties {
public:
    static VulkanCoreDeviceProperties QueryGpuProperties(VkPhysicalDevice _gpu, uint32_t _api_version);

    VkPhysicalDeviceProperties         core_1_0{};
    VkPhysicalDeviceVulkan11Properties core_1_1{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties core_1_2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties core_1_3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
};

class VulkanOptionalDeviceProperties {
public:
    // Descriptor binding
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer_properties{};
    VkPhysicalDevicePushDescriptorPropertiesKHR   push_descriptor_properties{};

    // Ray tracing
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR    ray_tracing_pipeline_properties{};

    // Mesh shading
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_shader_properties{};

    // Memory operations
    VkPhysicalDeviceCopyMemoryIndirectPropertiesNV  copy_memory_indirect_properties{};
    VkPhysicalDeviceMemoryDecompressionPropertiesNV memory_decompression_properties{};

    // Cooperative operations
    VkPhysicalDeviceCooperativeMatrixPropertiesKHR cooperative_matrix_properties{};
    VkPhysicalDeviceCooperativeVectorPropertiesNV  cooperative_vector_properties{};
    Array<VkCooperativeMatrixPropertiesKHR>        cooperative_matrix_supports{};
    Array<VkCooperativeVectorPropertiesNV>         cooperative_vector_supports{};
};

} // namespace Moer::Render

#endif // VULKAN_DEVICE_PROPERTY_H
