//
// Created by 74535 on 2023/9/30.
//

#ifndef VULKAN_DEBUG_H
#define VULKAN_DEBUG_H

#include "math/Base.h"

#include <vulkan.h>
#include <string>

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

    struct Debug {
        static PFN_vkCreateDebugUtilsMessengerEXT  vkCreateDebugUtilsMessengerEXT;
        static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
        static VkDebugUtilsMessengerEXT            debug_utils_messenger;

        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT             messageType,
            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void*                                       pUserData);
        static void SetupDebugUtilsMessengerEXT(VkInstance instance);
        static void DestroyDebugUtilsMessengerEXT(VkInstance instance);
    };

    struct DebugUtils {
        static PFN_vkCmdBeginDebugUtilsLabelEXT  vkCmdBeginDebugUtilsLabelEXT;
        static PFN_vkCmdEndDebugUtilsLabelEXT    vkCmdEndDebugUtilsLabelEXT;
        static PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT;

        static void Setup(VkInstance instance);
        static void CmdBeginLabel(VkCommandBuffer cmd_buffer, const std::string& caption, Moer::Vector4f color);
        static void CmdEndLabel(VkCommandBuffer cmd_buffer);
    };

}
}
}// namespace MoerEngine::RHI::Vulkan

#endif// !VULKAN_DEBUG_H
