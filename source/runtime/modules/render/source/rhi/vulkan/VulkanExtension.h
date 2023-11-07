//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_EXTENSION_H
#define VULKAN_EXTENSION_H

#include "rhi/vulkan/misc/VulkanTypeDefs.h"

class VulkanExtensionBase {
public:
    VulkanExtensionBase(const std::string& _ext_name) : m_extension_name(_ext_name) {}
    virtual ~VulkanExtensionBase() = default;

    inline const std::string GetExtensionName() const { return m_extension_name; }

protected:
    const std::string m_extension_name;
};

class VulkanInstanceExtension : public VulkanExtensionBase {
public:
    VulkanInstanceExtension(const std::string& _ext_name) : VulkanExtensionBase(_ext_name) {}
    virtual ~VulkanInstanceExtension() = default;

    static TExtensionArray      GetMESupportedInstanceExtensions();
    static TExtensionPropsArray GetDriverSupportedInstanceExtensions(const char* _layer_name = nullptr);
    static TExtensionArray      GetDriverSupportedInstanceExtensionNames(const char* _layer_name = nullptr);
};

class VulkanDeviceExtension : public VulkanExtensionBase {
public:
    VulkanDeviceExtension(const std::string& _ext_name) : VulkanExtensionBase(_ext_name) {}
    virtual ~VulkanDeviceExtension() = default;

    static TVulkanDeviceExtensionArray GetMESupportedDeviceExtensions();
    static TExtensionPropsArray        GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
    static TExtensionArray             GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);

    virtual void PreGpuFeatures(VkPhysicalDeviceFeatures2& _gpu_features2) {}
    virtual void PreGpuProperties(VkPhysicalDeviceProperties2& _gpu_properties2) {}
    virtual void PreCreateDevice(VkDeviceCreateInfo& _device_create_info) {}
};

#endif//VULKAN_EXTENSION_H
