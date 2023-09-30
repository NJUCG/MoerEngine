//
// Created by 74535 on 2023/9/30.
//

#include "VulkanDebug.h"
#include <sstream>
#include "spdlog/spdlog.h"

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugger::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT             messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void*                                       pUserData) {

    std::stringstream stream;
    stream << "[" << pCallbackData->messageIdNumber << "][" << pCallbackData->pMessageIdName << "]: " << pCallbackData->pMessage << std::endl;

    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        spdlog::debug(stream.str());
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        spdlog::info(stream.str());
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        spdlog::warn(stream.str());
    } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        spdlog::error(stream.str());
    }

    return VK_FALSE;
}

void VulkanDebugger::SetupDebugUtilsMessengerEXT(VkInstance instance) {
    vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

    if (vkCreateDebugUtilsMessengerEXT == nullptr || vkDestroyDebugUtilsMessengerEXT == nullptr) {
        assert(false);
    }

    VkDebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCreateInfo{};
    debugUtilsMessengerCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugUtilsMessengerCreateInfo.pNext = nullptr;
    debugUtilsMessengerCreateInfo.flags = 0;
    debugUtilsMessengerCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugUtilsMessengerCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugUtilsMessengerCreateInfo.pfnUserCallback = DebugCallback;
    debugUtilsMessengerCreateInfo.pUserData = nullptr;

    assert(vkCreateDebugUtilsMessengerEXT(instance, &debugUtilsMessengerCreateInfo, nullptr, &debugUtilsMessenger) == VK_SUCCESS);
}

void VulkanDebugger::DestroyDebugUtilsMessengerEXT(VkInstance instance) {
    if (debugUtilsMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance, debugUtilsMessenger, nullptr);
    }
}

}
}
}// namespace MoerEngine::RHI::Vulkan::Debug