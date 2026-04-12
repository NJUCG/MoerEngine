//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_GENERIC_PLATFORM_H
#define VULKAN_GENERIC_PLATFORM_H

#include "misc/STL.h"

#include <volk.h>

namespace Moer::Render {

class VulkanDevice;
class VulkanDeviceFeatures;

} // namespace Moer::Render

class VulkanGenericPlatform {
public:
    static void GetInstanceLayers(Moer::Array<std::string>& _layers) {}
    static void GetDeviceLayers(Moer::Array<std::string>& _layers) {}
    // create the platform-specific surface object - required
    static VkSurfaceKHR CreateSurface();
    // Allow the platform code to restrict the device features
    static void RestrictEnabledPhysicalDeviceFeatures(Moer::Render::VulkanDeviceFeatures* _gpu_features);
};

#endif // VULKAN_GENERIC_PLATFORM_H
