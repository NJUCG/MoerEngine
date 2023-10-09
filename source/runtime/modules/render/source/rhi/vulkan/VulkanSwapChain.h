//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "rhi/vulkan/windows/VulkanWindowsPlatform.h"
#include "VulkanDevice.h"

#include <vulkan.h>

#include <memory>
#include <vector>

struct SwapChainBuffer {
    VkImage     image;
    VkImageView view;
};

class VulkanSwapChain {
public:
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    void InitSurface(void* platform_handle, void* platform_window);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    void InitSurface(wl_display* display, wl_surface* window);
#endif
    void     Connect(VkInstance _instance, const std::shared_ptr<VulkanDevice>& _device);
    void     Init(uint32_t* width, uint32_t* height, bool vsync, bool fullscreen);
    uint32_t AcquireNextImage();
    void     Present(VkQueue _queue);
    void     Cleanup();

private:
    VkInstance                  m_instance;
    std::weak_ptr<VulkanDevice> m_device;
    VkSwapchainKHR              m_swap_chain;
    VkSurfaceKHR                m_surface;
    VkFormat                    m_color_format;
    VkColorSpaceKHR             m_color_space;

    std::vector<VkSemaphore> m_image_acquired_semaphores;
    std::vector<VkSemaphore> m_render_complete_semaphores;

    std::vector<VkImage>         images;
    std::vector<SwapChainBuffer> buffers;

    uint32_t current_image_index;
    uint32_t semaphore_index;

private:
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& _available_formats);
    VkPresentModeKHR   ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& _available_present_modes, bool vsync = false);
    VkExtent2D         ChooseSwapExtent(uint32_t* width, uint32_t* height, const VkSurfaceCapabilitiesKHR& _capabilities);
    void               ChoosePresentQueue();
};

#endif// VULKAN_SWAP_CHAIN_H
