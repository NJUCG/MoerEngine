//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_GENERIC_PLATFORM_H
#define VULKAN_GENERIC_PLATFORM_H

#include "../VulkanCommon.h"
#include "misc/STL.h"

namespace Moer::Render {

class VulkanDeviceFeatures;

class VulkanGenericPlatform {
public:
    static void GetInstanceLayers(Moer::Array<std::string>& _layers) {}
    static void GetDeviceLayers(Moer::Array<std::string>& _layers) {}
    // create the platform-specific surface object - required
    static VkSurfaceKHR CreateSurface();
    // Allow the platform code to restrict the device features
    static void RestrictEnabledPhysicalDeviceFeatures(VulkanDeviceFeatures* _gpu_features);
};

} // namespace Moer::Render

#endif // VULKAN_GENERIC_PLATFORM_H
