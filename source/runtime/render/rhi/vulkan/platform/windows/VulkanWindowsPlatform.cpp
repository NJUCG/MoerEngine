//
// Created by 74535 on 2023/10/1.
//
#include "VulkanWindowsPlatform.h"
#include "../../VulkanMacroUtils.h"

#include "../../VulkanExtension.h"
namespace Moer::Render {
    void VulkanWindowsPlatform::GetInstanceExtensions(TExtensionArray& _extensions) {
        _extensions.emplace_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        _extensions.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    }

    void VulkanWindowsPlatform::GetInstanceLayers(TLayerArray& _layers) {
#ifndef NDEBUG
        _layers.emplace_back("VK_LAYER_KHRONOS_validation");
#endif
    }

    void VulkanWindowsPlatform::GetDeviceExtensions(TVulkanDeviceExtensionArray& _extensions) {
        // _extensions.emplace_back(std::make_unique<VulkanDeviceExtension>(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME));
        // _extensions.emplace_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    }

    void VulkanWindowsPlatform::CreateSurface(void* _window_handle, VkInstance _instance, VkSurfaceKHR& _surface) {
        VkWin32SurfaceCreateInfoKHR surface_create_info{};
        surface_create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        //surface_create_info.hinstance = GetModuleHandle(nullptr);
        surface_create_info.hwnd = (HWND)_window_handle;
        VK_CHECK_RESULT(vkCreateWin32SurfaceKHR(_instance, &surface_create_info, nullptr, &_surface));
    }
}// namespace Moer::Render