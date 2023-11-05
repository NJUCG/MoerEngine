//
// Created by 74535 on 2023/10/11.
//

#include "VulkanExtension.h"
#include "VulkanPlatform.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "vulkan/vulkan_core.h"

TExtensionArray VulkanInstanceExtension::GetMESupportedInstanceExtensions() {
    TExtensionArray extensions;

#define ADD_EXTENSION(ext_name) extensions.push_back(ext_name)

    // generic simple extensions
    ADD_EXTENSION(VK_KHR_SURFACE_EXTENSION_NAME);
    ADD_EXTENSION(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

    // debug extensions
    ADD_EXTENSION(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    ADD_EXTENSION(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

    // platform specific extensions
    VulkanPlatform::GetInstanceExtensions(extensions);

    // custom extensions

#undef ADD_EXTENSION

    return extensions;
}

TExtensionPropsArray VulkanInstanceExtension::GetDriverSupportedInstanceExtensions(const char* _layer_name) {
    TExtensionPropsArray ext_props;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, ext_props.data()));
    }

    return ext_props;
}

TExtensionArray VulkanInstanceExtension::GetDriverSupportedInstanceExtensionNames(const char* _layer_name) {
    TExtensionPropsArray ext_props;
    TExtensionArray      extensions;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateInstanceExtensionProperties(_layer_name, &prop_count, ext_props.data()));
        for (const auto& prop : ext_props) {
            extensions.push_back(prop.extensionName);
        }
    }

    return extensions;
}

TExtensionArray VulkanDeviceExtension::GetMESupportedDeviceExtensions() {
    TExtensionArray extensions;

#define ADD_EXTENSION(ext_name) extensions.push_back(ext_name)
    // generic simple extensions
    ADD_EXTENSION(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // timeline semaphore extensions
    ADD_EXTENSION(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    // raytracing extensions

    // vendor extensions

    // debug extensions

    // platform specific extensions
    VulkanPlatform::GetDeviceExtensions(extensions);

#undef ADD_EXTENSION

    return extensions;
}

TExtensionPropsArray VulkanDeviceExtension::GetDriverSupportedDeviceExtensions(VkPhysicalDevice _gpu, const char* _layer_name) {
    TExtensionPropsArray ext_props;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, ext_props.data()));
    }

    return ext_props;
}

TExtensionArray VulkanDeviceExtension::GetDriverSupportedDeviceExtensionNames(VkPhysicalDevice _gpu, const char* _layer_name) {
    TExtensionPropsArray ext_props;
    TExtensionArray      extensions;

    uint32_t prop_count = 0;
    VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, nullptr));
    if (prop_count > 0) {
        ext_props.resize(prop_count);
        VK_CHECK_RESULT(vkEnumerateDeviceExtensionProperties(_gpu, _layer_name, &prop_count, ext_props.data()));
        for (const auto& prop : ext_props) {
            extensions.push_back(prop.extensionName);
        }
    }

    return extensions;
}
