//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_GENERIC_PLATFORM_H
#define VULKAN_GENERIC_PLATFORM_H

#include "misc/STL.h"

#include <vulkan/vulkan.h>

class VulkanDevice;
class VulkanPhysicalDeviceFeatures;

class VulkanGenericPlatform {
public:
    // Array of required extensions for the platform (Required!)
    static void GetInstanceExtensions(Moer::Array<std::string>& _extensions);
    static void GetInstanceLayers(Moer::Array<std::string>& _layers) {}
    static void GetDeviceExtensions(const VulkanDevice* _device, Moer::Array<std::string>& _extensions);
    static void GetDeviceLayers(Moer::Array<std::string>& _layers) {}
    // create the platform-specific surface object - required
    static VkSurfaceKHR CreateSurface();
    // Allow the platform code to restrict the device features
    static void RestrictEnabledPhysicalDeviceFeatures(VulkanPhysicalDeviceFeatures* _gpu_features);
};

#endif// VULKAN_GENERIC_PLATFORM_H
