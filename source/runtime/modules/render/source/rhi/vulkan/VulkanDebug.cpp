//
// Created by 74535 on 2023/9/30.
//

#include "misc/MacroUtils.h"
#include "VulkanDebug.h"
#include <sstream>
#include <spdlog/spdlog.h>

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

    namespace Debug {
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
            VkDebugUtilsMessageTypeFlagsEXT             message_type,
            const VkDebugUtilsMessengerCallbackDataEXT* p_callback_data,
            void*                                       p_user_data) {

            std::stringstream stream;
            stream << "[" << p_callback_data->messageIdNumber << "][" << p_callback_data->pMessageIdName << "]: " << p_callback_data->pMessage << std::endl;

            if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
                MOER_LOG_DEBUG(stream.str());
            } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
                MOER_LOG_INFO(stream.str());
            } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                MOER_LOG_WARN(stream.str());
            } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                MOER_LOG_ERROR(stream.str());
            }

            return VK_FALSE;
        }

        void SetupDebugUtilsMessengerEXT(VkInstance instance) {
            vk_create_debug_utils_messenger_ext  = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            vk_destroy_debug_utils_messenger_ext = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

            if (vk_create_debug_utils_messenger_ext == nullptr || vk_destroy_debug_utils_messenger_ext == nullptr) {
                assert(false);
            }

            VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info{};
            debug_utils_messenger_create_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debug_utils_messenger_create_info.pNext           = nullptr;
            debug_utils_messenger_create_info.flags           = 0;
            debug_utils_messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug_utils_messenger_create_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug_utils_messenger_create_info.pfnUserCallback = DebugCallback;
            debug_utils_messenger_create_info.pUserData       = nullptr;

            assert(vkCreateDebugUtilsMessengerEXT(instance, &debug_utils_messenger_create_info, nullptr, &debug_utils_messenger) == VK_SUCCESS);
        }

        void DestroyDebugUtilsMessengerEXT(VkInstance instance) {
            if (debug_utils_messenger != VK_NULL_HANDLE) {
                vkDestroyDebugUtilsMessengerEXT(instance, debug_utils_messenger, nullptr);
            }
        }
    }// namespace Debug

    namespace DebugUtils {
        void Setup(VkInstance instance) {
            vk_cmd_begin_debug_utils_label_ext  = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT"));
            vk_cmd_end_debug_utils_label_ext    = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT"));
            vk_cmd_insert_debug_utils_label_ext = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(vkGetInstanceProcAddr(instance, "vkCmdInsertDebugUtilsLabelEXT"));
        }

        void CmdBeginLabel(VkCommandBuffer cmd_buffer, const std::string& caption, glm::vec4 color) {
            if (!vk_cmd_begin_debug_utils_label_ext) {
                return;
            }
            VkDebugUtilsLabelEXT label_info{};
            label_info.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label_info.pLabelName = caption.c_str();
            memcpy(label_info.color, &color[0], sizeof(float) * 4);
            vkCmdBeginDebugUtilsLabelEXT(cmd_buffer, &label_info);
        }

        void CmdEndLabel(VkCommandBuffer cmd_buffer) {
            if (!vk_cmd_end_debug_utils_label_ext) {
                return;
            }
            vkCmdEndDebugUtilsLabelEXT(cmd_buffer);
        }
    }// namespace DebugUtils

}
}
}// namespace MoerEngine::RHI::Vulkan