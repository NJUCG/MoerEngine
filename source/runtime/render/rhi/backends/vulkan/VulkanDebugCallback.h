#pragma once

#include "VulkanCommon.h"

namespace Moer::Render {

// flush function to be called periodically (e.g., end of frame) or at program exit
void FlushBufferedDebugMessages();

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    VkDebugUtilsMessageTypeFlagsEXT             message_type,
    const VkDebugUtilsMessengerCallbackDataEXT* p_callback_data,
    void*                                       p_user_data
);

} // namespace Moer::Render
