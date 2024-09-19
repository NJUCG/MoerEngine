//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_UTIL_H
#define VULKAN_UTIL_H

#include "misc/STL.h"

#include <vulkan/vulkan.h>
namespace Moer {
namespace RHI {
namespace Vulkan {

namespace Util {
    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR        capabilities;
        Moer::Array<VkSurfaceFormatKHR> formats;
        Moer::Array<VkPresentModeKHR>   present_modes;
    };
    /** @brief Disable message boxes on fatal errors */
    extern bool error_mode_silent;

    /** @brief Returns an error code as a string */
    std::string ErrorString(VkResult error_code);

    /** @brief Returns the device type as a string */
    std::string PhysicalDeviceTypeString(VkPhysicalDeviceType type);

    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface);

    template<typename PropType, VkStructureType sType>
    PropType QueryPhysicalDeviceExtensionProps(VkPhysicalDevice _gpu) {
        PropType request_prop{};
        request_prop.sType = sType;
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &request_prop;
        vkGetPhysicalDeviceProperties2(_gpu, &props);
        return request_prop;
    }

    // Display error message and exit on fatal error
    void ExitFatal(const std::string& message, int32_t exit_code);
    void ExitFatal(const std::string& message, VkResult result_code);

    uint32_t MemCrc32(const void* data, size_t data_size);
}

}
}
}// namespace Moer::RHI::Vulkan::Util

#endif// !VULKAN_UTIL_H
