//
// Created by 74535 on 2023/9/30.
//

#ifndef VULKAN_DEBUG_H
#define VULKAN_DEBUG_H

#include <vulkan.h>
namespace MoerEngine {
namespace RHI {
namespace Vulkan {

struct VulkanDebugger {
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;
    VkDebugUtilsMessengerEXT debugUtilsMessenger;

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT             messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void*                                       pUserData);

    void SetupDebugUtilsMessengerEXT(VkInstance instance);

    void DestroyDebugUtilsMessengerEXT(VkInstance instance);

};

}
}
}// namespace MoerEngine::RHI::Vulkan::Debug

#endif// !VULKAN_DEBUG_H
