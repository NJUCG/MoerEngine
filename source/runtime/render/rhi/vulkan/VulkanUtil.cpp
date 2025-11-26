//
// Created by 74535 on 2023/10/1.
//

#include "misc/Crc32.h"

#include "VulkanMacroUtils.h"
#include "VulkanUtil.h"
#include <volk.h>

namespace Moer { namespace RHI { namespace Vulkan { namespace Util {

SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) {
    SwapChainSupportDetails details;

    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_gpu, _surface, &details.capabilities));

    uint32_t format_count = 0;
    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, nullptr));
    if (format_count > 0) {
        details.formats.resize(format_count);
        VK_CHECK_RESULT(
            vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, details.formats.data())
        );
    }

    uint32_t present_mode_count = 0;
    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, nullptr));
    if (present_mode_count > 0) {
        details.present_modes.resize(present_mode_count);
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(
            _gpu, _surface, &present_mode_count, details.present_modes.data()
        ));
    }

    return details;
}

uint32_t MemCrc32(const void* data, size_t data_size) {
    return crc32_8bytes(data, data_size);
}
}}}} // namespace Moer::RHI::Vulkan::Util