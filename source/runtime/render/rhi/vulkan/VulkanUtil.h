//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_UTIL_H
#define VULKAN_UTIL_H

#include "misc/STL.h"

#include "VulkanPlatform.h"

namespace Moer { namespace RHI { namespace Vulkan { namespace Util {
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities;
    Moer::Array<VkSurfaceFormatKHR> formats;
    Moer::Array<VkPresentModeKHR>   present_modes;
};

SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface);

uint32_t MemCrc32(const void* data, size_t data_size);
}}}} // namespace Moer::RHI::Vulkan::Util

#endif // !VULKAN_UTIL_H
