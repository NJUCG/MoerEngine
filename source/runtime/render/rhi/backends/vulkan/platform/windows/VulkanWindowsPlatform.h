//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_WINDOWS_PLATFORM_H
#define VULKAN_WINDOWS_PLATFORM_H

#include "../../VulkanTypeDefs.h"
#include "../VulkanGenericPlatform.h"

namespace Moer::Render {
class VulkanWindowsPlatform : public VulkanGenericPlatform {
public:
    static void GetInstanceLayers(TLayerArray& _layers);
    static void GetDeviceLayers(TLayerArray& _layers) {}
};

using VulkanPlatform = VulkanWindowsPlatform;

} // namespace Moer::Render
#endif // VULKAN_WINDOWS_PLATFORM_H
