#ifndef VULKAN_EXTENSION_FACTORIES_H
#define VULKAN_EXTENSION_FACTORIES_H

#include "VulkanExtension.h"

namespace Moer::Render {

std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTSwapchainMaintenance1Extension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRAccelerationStructureExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRRayTracingPipelineExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRRayQueryExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRComputeShaderDerivativesExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRShaderUntypedPointersExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTDescriptorHeapExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTDescriptorBufferExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRPushDescriptorExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTMemoryDecompressionExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTCopyMemoryIndirectExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTMemoryPriorityAllocateInfoExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTPageableDeviceLocalMemoryExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanEXTMeshShaderExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanKHRCooperativeMatrixExtension(bool _optional);
std::shared_ptr<VulkanDeviceExtension> CreateVulkanNVCooperativeVectorExtension(bool _optional);

} // namespace Moer::Render

#endif // VULKAN_EXTENSION_FACTORIES_H
