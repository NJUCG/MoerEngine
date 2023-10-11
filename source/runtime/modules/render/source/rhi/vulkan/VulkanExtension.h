//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_EXTENSION_H
#define VULKAN_EXTENSION_H

#include "rhi/vulkan/misc/VulkanTypeDefs.h"

class VulkanInstanceExtension {
public:
    static TExtensionArray      GetMESupportedInstanceExtensions();
    static TExtensionPropsArray GetDriverSupportedInstanceExtensions(const char* _layer_name = nullptr);
    static TExtensionArray      GetDriverSupportedInstanceExtensionNames(const char* _layer_name = nullptr);
};

class VulkanDeviceExtension {
public:
    static TExtensionArray      GetMESupportedDeviceExtensions();
    static TExtensionPropsArray GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
    static TExtensionArray      GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
};

#endif//VULKAN_EXTENSION_H
