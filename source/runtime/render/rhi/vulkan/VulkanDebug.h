//
// Created by 74535 on 2023/9/30.
//

#ifndef VULKAN_DEBUG_H
#define VULKAN_DEBUG_H

#include "math/Base.h"

#include <volk.h>

#include <string>

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

struct DebugUtils {
    static PFN_vkCmdBeginDebugUtilsLabelEXT  vkCmdBeginDebugUtilsLabelEXT;
    static PFN_vkCmdEndDebugUtilsLabelEXT    vkCmdEndDebugUtilsLabelEXT;
    static PFN_vkCmdInsertDebugUtilsLabelEXT vkCmdInsertDebugUtilsLabelEXT;
    static PFN_vkSetDebugUtilsObjectNameEXT  vkSetDebugUtilsObjectNameEXT;

    static void Setup(VkInstance instance);
    static void CmdBeginLabel(VkCommandBuffer cmd_buffer, const std::string& caption, Moer::Vector4f color);
    static void CmdInsertLabel(VkCommandBuffer cmd_buffer, const std::string& caption, Moer::Vector4f color);
    static void CmdEndLabel(VkCommandBuffer cmd_buffer);
    static void
    SetObjectName(VkDevice device, uint64_t object, VkObjectType object_type, const std::string& name);
};

}}} // namespace Moer::RHI::Vulkan

#endif // !VULKAN_DEBUG_H
