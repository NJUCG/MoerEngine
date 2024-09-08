//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_WINDOWS_PLATFORM_H
#define VULKAN_WINDOWS_PLATFORM_H

#define VK_USE_PLATFORM_WIN32_KHR 1
#define VK_USE_PLATFORM_WIN32_KHX 1

#define VULKAN_RHI_RAYTRACING               (RHI_RAYTRACING)
#define VULKAN_SUPPORTS_SCALAR_BLOCK_LAYOUT (VULKAN_RHI_RAYTRACING)

#if VULKAN_RHI_RAYTRACING
#define VK_API_VERSION VK_API_VERSION_1_3
#else
#define VK_API_VERSION VK_API_VERSION_1_1
#endif// VULKAN_RHI_RAYTRACING

// and now, include the GenericPlatform class
#include "../../VulkanTypeDefs.h"
#include "../VulkanGenericPlatform.h"

namespace Moer::Render {
    class VulkanWindowsPlatform : public VulkanGenericPlatform {
    public:
        // Array of required extensions for the platform (Required!)
        static void GetInstanceExtensions(TExtensionArray& _extensions);
        static void GetInstanceLayers(TLayerArray& _layers) {}
        static void GetDeviceExtensions(TVulkanDeviceExtensionArray& _extensions);
        static void GetDeviceLayers(TLayerArray& _layers) {}
        // create the platform-specific surface object - required
        static void CreateSurface(void* _window_handle, VkInstance _instance, VkSurfaceKHR& _surface);
    };

}// namespace Moer::Render

using VulkanPlatform = Moer::Render::VulkanWindowsPlatform;
#endif// VULKAN_WINDOWS_PLATFORM_H
