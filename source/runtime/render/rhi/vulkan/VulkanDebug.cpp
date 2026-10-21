//
// Created by 74535 on 2023/9/30.
//

#include "VulkanDebug.h"
#include "log/LogSystem.h"
#include <cassert>
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

}}} // namespace Moer::RHI::Vulkan