//
// Created by 74535 on 2023/10/5.
//

#include "VulkanSwapChain.h"
#include "misc/VulkanMacroUtils.h"

#if defined(VK_USE_PLATFORM_WIN32_KHR)
void VulkanSwapChain::InitSurface(void* platform_handle, void* platform_window) {
}
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
void VulkanSwapChain::InitSurface(wl_display* display, wl_surface* window) {
}
#endif

void VulkanSwapChain::Connect(VkInstance _instance, const std::shared_ptr<VulkanDevice>& _device) {
    m_instance = _instance;
    m_device   = _device;
}

/**
 * Initialize the swapchain and get its images with given width and height
 * @param width Pointer to the width of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param height Pointer to the height of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param vsync (Optional) Can be used to force vsync-ed rendering (by using VK_PRESENT_MODE_FIFO_KHR as presentation mode)
 * @param fullscreen
 */
void VulkanSwapChain::Init(uint32_t* width, uint32_t* height, bool vsync, bool fullscreen) {
    VkSwapchainKHR old_swap_chain = m_swap_chain;

    auto device = m_device.lock();

    auto details        = QuerySwapChainSupport(device->GetGpu());
    auto surface_format = ChooseSwapSurfaceFormat(details.formats);
    auto present_mode   = ChooseSwapPresentMode(details.present_modes, vsync);
    auto extent         = ChooseSwapExtent(width, height, details.capabilities);

    // set the number of images
    uint32_t image_count = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && image_count > details.capabilities.maxImageCount) {
        image_count = details.capabilities.maxImageCount;
    }
}

uint32_t VulkanSwapChain::AcquireNextImage() {
    return 0;
}

void VulkanSwapChain::Present(VkQueue _queue) {
}

void VulkanSwapChain::Cleanup() {
}

VkSurfaceFormatKHR VulkanSwapChain::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& _available_formats) {
    for (const auto& format : _available_formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    // fallback
    return _available_formats[0];
}

VkPresentModeKHR VulkanSwapChain::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& _available_present_modes, bool vsync) {
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        for (auto mode : _available_present_modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                present_mode = mode;
                break;
            }
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                present_mode = mode;
            }
        }
    }
    return present_mode;
}

VkExtent2D VulkanSwapChain::ChooseSwapExtent(uint32_t* width, uint32_t* height, const VkSurfaceCapabilitiesKHR& _capabilities) {
    if (_capabilities.currentExtent.width != static_cast<uint32_t>(-1)) {
        *width  = _capabilities.currentExtent.width;
        *height = _capabilities.currentExtent.height;
        return _capabilities.currentExtent;
    }
    VkExtent2D extent = {static_cast<uint32_t>(*width), static_cast<uint32_t>(*height)};
    extent.width      = std::clamp(extent.width, _capabilities.minImageExtent.width, _capabilities.maxImageExtent.width);
    extent.height     = std::clamp(extent.height, _capabilities.minImageExtent.height, _capabilities.maxImageExtent.height);

    return extent;
}

void VulkanSwapChain::ChoosePresentQueue() {
    auto device = m_device.lock();
}
