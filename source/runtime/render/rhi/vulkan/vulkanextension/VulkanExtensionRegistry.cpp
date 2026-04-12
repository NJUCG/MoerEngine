#include "VulkanExtensionRegistry.h"
#include "VulkanExtensionFactories.h"
#include "platform/Platform.h"

#if PLATFORM_WINDOWS
#include "../platform/windows/VulkanWindowsPlatform.h"
#endif

namespace Moer::Render {

static constexpr VulkanExtensionDesc vulkan_extension_descs[] = {
    {
        .kind     = EVulkanExtensionKind::Instance,
        .name     = VK_KHR_SURFACE_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Instance,
        .name     = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Instance,
        .name     = VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    // {
    //     .kind     = EVulkanExtensionKind::Device,
    //     .name     = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
    //     .optional = false,
    //     .factory  = &CreateVulkanEXTSwapchainMaintenance1Extension,
    // },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanEXTDescriptorBufferExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        .optional = false,
        .factory  = &CreateVulkanKHRPushDescriptorExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanEXTMemoryPriorityAllocateInfoExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanEXTPageableDeviceLocalMemoryExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_NV_MEMORY_DECOMPRESSION_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanEXTMemoryDecompressionExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_NV_COPY_MEMORY_INDIRECT_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanEXTCopyMemoryIndirectExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanKHRCooperativeMatrixExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanNVCooperativeVectorExtension,
    },
#if VULKAN_RHI_RAYTRACING
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        .optional = true,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanKHRAccelerationStructureExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanKHRRayTracingPipelineExtension,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_RAY_QUERY_EXTENSION_NAME,
        .optional = true,
        .factory  = &CreateVulkanKHRRayQueryExtension,
    },
#endif
#if PLATFORM_WINDOWS
    {
        .kind     = EVulkanExtensionKind::Instance,
        .name     = VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Instance,
        .name     = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
#endif
#if PLATFORM_WINDOWS && WITH_CUDA
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
    {
        .kind     = EVulkanExtensionKind::Device,
        .name     = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        .optional = false,
        .factory  = nullptr,
    },
#endif
};

std::span<const VulkanExtensionDesc> GetVulkanExtensionDescs() {
    return vulkan_extension_descs;
}

} // namespace Moer::Render