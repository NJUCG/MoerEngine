//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "VulkanDevice.h"
#include "misc/CountableRef.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "vulkan/vulkan_core.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

struct SwapChainBuffer {
    // VkImage image;
    class VulkanRHITexture* image;
    // VkImageView view;
    class VulkanImageView* view;
};

class VulkanSwapChain {
public:
    ~VulkanSwapChain();
    void     Connect(VkInstance _instance, VkSurfaceKHR _surface, VulkanDevice* _device);
    void     Init(uint32_t* width, uint32_t* height, bool vsync);
    uint32_t AcquireNextImage(VkSemaphore _aquire_semaphore);
    VkResult Present(VkQueue _queue, VkSemaphore _render_finished);
    void     Cleanup();

    VkSurfaceFormatKHR GetSurfaceFormat() const { return surface_format; }

private:
    void Create(uint32_t* width, uint32_t* height, bool vsync);
    void Recreate();

private:
    friend class VulkanViewport;
    friend class VulkanRHIImpl;
    VkInstance     m_instance;
    VulkanDevice*  m_device;
    VkSwapchainKHR m_swap_chain;
    VkSurfaceKHR   m_surface;

    std::vector<VkImage> m_swap_chain_images;

    uint32_t current_image_index;

    VkExtent2D         extent;
    VkFormat           image_format;
    VkSurfaceFormatKHR surface_format;

private:
    VkImageView CreateImageView(VkImage _image, VkFormat _format, uint32_t mipLevels, VkImageAspectFlags aspectMask);

    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& _available_formats);
    VkPresentModeKHR   ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& _available_present_modes, bool vsync = false);
    VkExtent2D         ChooseSwapExtent(uint32_t* width, uint32_t* height, const VkSurfaceCapabilitiesKHR& _capabilities);
};

#endif// VULKAN_SWAP_CHAIN_H
