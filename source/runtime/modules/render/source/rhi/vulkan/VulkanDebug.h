//
// Created by 74535 on 2023/9/30.
//

#ifndef VULKAN_DEBUG_H
#define VULKAN_DEBUG_H

#include <vulkan.h>
#include <glm/glm.hpp>

#include <string>

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

    namespace Debug {
        PFN_vkCreateDebugUtilsMessengerEXT  vk_create_debug_utils_messenger_ext;
        PFN_vkDestroyDebugUtilsMessengerEXT vk_destroy_debug_utils_messenger_ext;
        VkDebugUtilsMessengerEXT            debug_utils_messenger;

        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT             messageType,
            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void*                                       pUserData);
        void SetupDebugUtilsMessengerEXT(VkInstance instance);
        void DestroyDebugUtilsMessengerEXT(VkInstance instance);
    }// namespace Debug

    namespace DebugUtils {
        PFN_vkCmdBeginDebugUtilsLabelEXT  vk_cmd_begin_debug_utils_label_ext{nullptr};
        PFN_vkCmdEndDebugUtilsLabelEXT    vk_cmd_end_debug_utils_label_ext{nullptr};
        PFN_vkCmdInsertDebugUtilsLabelEXT vk_cmd_insert_debug_utils_label_ext{nullptr};

        void Setup(VkInstance instance);
        void CmdBeginLabel(VkCommandBuffer cmd_buffer, const std::string& caption, glm::vec4 color);
        void CmdEndLabel(VkCommandBuffer cmd_buffer);
    }// namespace DebugUtils

}
}
}// namespace MoerEngine::RHI::Vulkan

#endif// !VULKAN_DEBUG_H
