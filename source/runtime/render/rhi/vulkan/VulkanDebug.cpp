//
// Created by 74535 on 2023/9/30.
//

#include "VulkanDebug.h"
#include "log/LogSystem.h"
#include <sstream>

namespace Moer { namespace RHI { namespace Vulkan {

PFN_vkCreateDebugUtilsMessengerEXT  Debug::vkCreateDebugUtilsMessengerEXT  = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT Debug::vkDestroyDebugUtilsMessengerEXT = nullptr;
VkDebugUtilsMessengerEXT            Debug::debug_utils_messenger           = VK_NULL_HANDLE;

VKAPI_ATTR VkBool32 VKAPI_CALL Debug::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    VkDebugUtilsMessageTypeFlagsEXT             message_type,
    const VkDebugUtilsMessengerCallbackDataEXT* p_callback_data,
    void*                                       p_user_data
) {

    std::stringstream stream;
    stream << "[" << p_callback_data->messageIdNumber << "][" << p_callback_data->pMessageIdName
           << "]: " << p_callback_data->pMessage << std::endl;

    if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        LOG_DEBUG(stream.str());
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LOG_INFO(stream.str());
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARNING(stream.str());
    } else if (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_ERROR(stream.str());
    }

    return VK_FALSE;
}

void Debug::SetupDebugUtilsMessengerEXT(VkInstance instance) {
    vkCreateDebugUtilsMessengerEXT =
        PFN_vkCreateDebugUtilsMessengerEXT(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    vkDestroyDebugUtilsMessengerEXT =
        PFN_vkDestroyDebugUtilsMessengerEXT(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
        );

    if (vkCreateDebugUtilsMessengerEXT == nullptr || vkDestroyDebugUtilsMessengerEXT == nullptr) {
        assert(false);
    }

    VkDebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info{};
    PopulateDebugMessengerCreateInfo(debug_utils_messenger_create_info);

    assert(
        vkCreateDebugUtilsMessengerEXT(
            instance, &debug_utils_messenger_create_info, nullptr, &debug_utils_messenger
        ) == VK_SUCCESS
    );
}

void Debug::DestroyDebugUtilsMessengerEXT(VkInstance instance) {
    if (debug_utils_messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance, debug_utils_messenger, nullptr);
    }
}

void Debug::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& create_info) {
    create_info = {};

    create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    create_info.pNext = nullptr;
    create_info.flags = 0;
    create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    create_info.pfnUserCallback = DebugCallback;
    create_info.pUserData       = nullptr;
}

PFN_vkCmdBeginDebugUtilsLabelEXT  DebugUtils::vkCmdBeginDebugUtilsLabelEXT  = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT    DebugUtils::vkCmdEndDebugUtilsLabelEXT    = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT DebugUtils::vkCmdInsertDebugUtilsLabelEXT = nullptr;
PFN_vkSetDebugUtilsObjectNameEXT  DebugUtils::vkSetDebugUtilsObjectNameEXT  = nullptr;

void DebugUtils::Setup(VkInstance instance) {
    vkCmdBeginDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT")
    );
    vkCmdEndDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT")
    );
    vkCmdInsertDebugUtilsLabelEXT = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        vkGetInstanceProcAddr(instance, "vkCmdInsertDebugUtilsLabelEXT")
    );
    vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT")
    );
}

void DebugUtils::CmdBeginLabel(VkCommandBuffer cmd_buffer, const std::string& caption, Moer::Vector4f color) {
    if (!vkCmdBeginDebugUtilsLabelEXT) {
        return;
    }
    VkDebugUtilsLabelEXT label_info{};
    label_info.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label_info.pLabelName = caption.c_str();
    memcpy(label_info.color, &color[0], sizeof(float) * 4);
    vkCmdBeginDebugUtilsLabelEXT(cmd_buffer, &label_info);
}

void DebugUtils::CmdInsertLabel(
    VkCommandBuffer    cmd_buffer,
    const std::string& caption,
    Moer::Vector4f     color
) {
    if (!vkCmdInsertDebugUtilsLabelEXT) {
        return;
    }
    VkDebugUtilsLabelEXT label_info{};
    label_info.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label_info.pLabelName = caption.c_str();
    memcpy(label_info.color, &color[0], sizeof(float) * 4);
    vkCmdInsertDebugUtilsLabelEXT(cmd_buffer, &label_info);
}

void DebugUtils::CmdEndLabel(VkCommandBuffer cmd_buffer) {
    if (!vkCmdEndDebugUtilsLabelEXT) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(cmd_buffer);
}

void DebugUtils::SetObjectName(
    VkDevice           device,
    uint64_t           object,
    VkObjectType       object_type,
    const std::string& name
) {
    if (!vkSetDebugUtilsObjectNameEXT) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT name_info{};
    name_info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType   = object_type;
    name_info.objectHandle = object;
    name_info.pObjectName  = name.c_str();
    vkSetDebugUtilsObjectNameEXT(device, &name_info);
}

}}} // namespace Moer::RHI::Vulkan