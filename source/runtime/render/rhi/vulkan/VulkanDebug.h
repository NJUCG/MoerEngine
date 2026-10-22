//
// Created by 74535 on 2023/9/30.
//

#ifndef VULKAN_DEBUG_H
#define VULKAN_DEBUG_H

#include "VulkanCommon.h"

namespace Moer { namespace RHI { namespace Vulkan {

struct Debug {
    static PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
    static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
    static VkDebugUtilsMessengerEXT            debug_utils_messenger;

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT             messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void*                                       pUserData
    );
    static void SetupDebugUtilsMessengerEXT(VkInstance instance);
    static void DestroyDebugUtilsMessengerEXT(VkInstance instance);
    static void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& create_info);
};

}}} // namespace Moer::RHI::Vulkan

#endif // !VULKAN_DEBUG_H
