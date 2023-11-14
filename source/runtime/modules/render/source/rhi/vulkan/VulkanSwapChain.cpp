//
// Created by 74535 on 2023/10/5.
//

#include "PixelFormat.h"
#include "VulkanUtil.h"
#include "VulkanSwapChain.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "vulkan/vulkan_core.h"
#include "VulkanRHIResource.h"

#include <stdint.h>

namespace VkUtil = Moer::RHI::Vulkan::Util;

void VulkanSwapChain::Connect(VkInstance _instance, VkSurfaceKHR _surface, VulkanDevice* _device) {
    m_instance = _instance;
    m_surface  = _surface;
    m_device   = _device;
}

/**
 * Initialize the swapchain and get its images with given width and height
 * @param width Pointer to the width of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param height Pointer to the height of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param vsync (Optional) Can be used to force vsync-ed rendering (by using VK_PRESENT_MODE_FIFO_KHR as presentation mode)
 * @param fullscreen
 */
void VulkanSwapChain::Init(uint32_t* width, uint32_t* height, uint32_t max_frame_in_flight, bool vsync) {
    Create(width, height, vsync);
}
void VulkanSwapChain::Create(uint32_t* width, uint32_t* height, bool vsync) {
    auto* device = m_device;

    auto details      = VkUtil::QuerySwapChainSupport(device->GetGpu(), m_surface);
    surface_format    = ChooseSwapSurfaceFormat(details.formats);
    auto present_mode = ChooseSwapPresentMode(details.present_modes, true);
    extent            = ChooseSwapExtent(width, height, details.capabilities);
    extent.width      = *width;
    extent.height     = *height;
    // set the number of images
    uint32_t image_count = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && image_count > details.capabilities.maxImageCount) {
        image_count = details.capabilities.maxImageCount;
    }

    image_format = surface_format.format;

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface          = m_surface;
    create_info.minImageCount    = image_count;
    create_info.imageFormat      = image_format;
    create_info.imageColorSpace  = surface_format.colorSpace;
    create_info.imageExtent      = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    auto indices = device->GetQueueFamilyIndices();

    std::vector<uint32_t> swap_chain_queue_family_indices = {indices.graphics.value(), indices.present.value()};
    if (indices.graphics != indices.present) {
        create_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = swap_chain_queue_family_indices.size();
        create_info.pQueueFamilyIndices   = swap_chain_queue_family_indices.data();
    } else {
        create_info.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
        create_info.queueFamilyIndexCount = 0;
    }

    create_info.preTransform   = details.capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode    = present_mode;
    create_info.clipped        = VK_TRUE;
    create_info.oldSwapchain   = VK_NULL_HANDLE;

    VK_CHECK_RESULT(vkCreateSwapchainKHR(*device, &create_info, nullptr, &m_swap_chain));

    vkGetSwapchainImagesKHR(*device, m_swap_chain, &image_count, nullptr);
    m_swap_chain_images.resize(image_count);
    vkGetSwapchainImagesKHR(*device, m_swap_chain, &image_count, m_swap_chain_images.data());
    current_image_index = 0;
    LOG_INFO("Vulkan swapchain initialized with {} images.", image_count);
}
void VulkanSwapChain::Recreate() {
    LOG_DEBUG("Try to recreate swapchain");
    vkQueueWaitIdle(m_device->GetPresentQueue());
    Cleanup();
    uint32_t width, height;
    Create(&width, &height, true);
}
uint32_t VulkanSwapChain::AcquireNextImage(VkSemaphore _aquire_semaphore) {
    uint32_t                  image_index = 0;
    VkAcquireNextImageInfoKHR aquire_info{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};
    aquire_info.swapchain = m_swap_chain;
    // VkSemaphore aquire_semaphore = m_image_acquired_semaphores[current_frame_offset];
    aquire_info.semaphore = _aquire_semaphore;

    VkResult result = vkAcquireNextImageKHR(m_device->GetDevice(), m_swap_chain, UINT64_MAX, aquire_info.semaphore, VK_NULL_HANDLE, &image_index);
    // VkResult result = vkAcquireNextImage2KHR(m_device->GetDevice(), &aquire_info, &image_index);
    //probably caused by resize
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        //not to render this frame
        Recreate();
        image_index = INT32_MAX;
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return image_index;
    }
    assert(false && "Error acquiring next present texture.");
    return INT32_MAX;
}

void VulkanSwapChain::Present(VkQueue _queue, VkSemaphore _render_finished) {
    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &_render_finished;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &m_swap_chain;
    present_info.pImageIndices      = &current_image_index;

    VkResult result = vkQueuePresentKHR(_queue, &present_info);

    current_image_index = (current_image_index + 1) % m_swap_chain_images.size();
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        Recreate();
    } else if (result != VK_SUCCESS) {
        assert(false && "Error presenting to swapchain.");
    }
}

void VulkanSwapChain::Cleanup() {

    vkDestroySwapchainKHR(m_device->GetDevice(), m_swap_chain, VK_NULL_HANDLE);
}

VkImageView VulkanSwapChain::CreateImageView(VkImage _image, VkFormat _format, uint32_t _mip_levels, VkImageAspectFlags _aspect_mask) {
    auto* device = m_device;

    VkImageView view;

    VkImageViewCreateInfo create_info{};
    create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    create_info.image                           = _image;
    create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    create_info.format                          = _format;
    create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    create_info.subresourceRange.aspectMask     = _aspect_mask;
    create_info.subresourceRange.baseMipLevel   = 0;
    create_info.subresourceRange.levelCount     = _mip_levels;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount     = 1;

    VK_CHECK_RESULT(vkCreateImageView(*device, &create_info, nullptr, &view));

    return view;
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
