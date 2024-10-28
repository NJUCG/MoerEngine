//
// Created by 74535 on 2023/10/5.
//

#include "PixelFormat.h"
#include "VulkanUtil.h"
#include "VulkanSwapChain.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/vulkan/VulkanRHI.h"

#include <volk.h>
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanCommand.h"
#include "window/WindowContext.h"
#include "VulkanDevice.h"

#include <stdint.h>

namespace VkUtil = Moer::RHI::Vulkan::Util;
namespace Moer::Render {
    void VulkanSwapChain::Connect(VkInstance _instance, VkSurfaceKHR _surface, VulkanDevice* _device) {
        m_instance = _instance;
        m_surface  = _surface;
        m_device   = _device;
    }
    VulkanSwapChain::VulkanSwapChain(VkInstance _instance, Moer::WindowHandle* _window, VulkanDevice* _device) : m_device(_device), m_instance(_instance) {
        m_instance = _instance;
        m_device   = _device;
        Moer::WindowContext::CreateVulkanSurface(_instance, _window, VK_NULL_HANDLE, &m_surface);
    }

    VulkanSwapChain::~VulkanSwapChain() {
        Cleanup();
    }

    /**
 * Initialize the swapchain and get its images with given width and height
 * @param width Pointer to the width of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param height Pointer to the height of the swapchain (may be adjusted to fit the requirements of the swapchain)
 * @param vsync (Optional) Can be used to force vsync-ed rendering (by using VK_PRESENT_MODE_FIFO_KHR as presentation mode)
 * @param fullscreen
 */
    void VulkanSwapChain::Init(uint32_t* width, uint32_t* height, bool vsync) {
        Create(width, height, vsync);
    }
    void VulkanSwapChain::Create(uint32_t* width, uint32_t* height, bool vsync) {
        auto* device = m_device;

        auto details      = VkUtil::QuerySwapChainSupport(device->GetGpu(), m_surface);
        surface_format    = ChooseSwapSurfaceFormat(details.formats);
        auto present_mode = ChooseSwapPresentMode(details.present_modes, vsync);
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
        create_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        auto indices = device->GetQueueFamilyIndices();

        Moer::Array<uint32_t> swap_chain_queue_family_indices = {indices.graphics.value(), indices.present.value()};
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

        VK_CHECK_RESULT(vkCreateSwapchainKHR(device->GetDevice(), &create_info, nullptr, &m_swap_chain));

        vkGetSwapchainImagesKHR(device->GetDevice(), m_swap_chain, &image_count, nullptr);
        m_swap_chain_images.resize(image_count);
        vkGetSwapchainImagesKHR(device->GetDevice(), m_swap_chain, &image_count, m_swap_chain_images.data());
        current_image_index = 0;

        // VulkanRHIImpl*  rhi_impl       = (VulkanRHIImpl*)g_rhi;
        // VkCommandPool   temp_pool      = m_device->GetCurrentCommandAllocator()->GetHandle(ECommandListType::COPY);
        // VkCommandBuffer command_buffer = rhi_impl->BeginSingleTimeCommands(temp_pool);

        // VkDependencyInfoKHR dependency_info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};

        // Moer::Array<VkImageMemoryBarrier2> barriers(m_swap_chain_images.size());
        // for (int i = 0; i < barriers.size(); i++) {
        //     barriers[i].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        //     barriers[i].pNext                           = nullptr;
        //     barriers[i].oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        //     barriers[i].newLayout                       = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        //     barriers[i].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        //     barriers[i].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        //     barriers[i].image                           = m_swap_chain_images[i];
        //     barriers[i].subresourceRange.baseMipLevel   = 0;
        //     barriers[i].subresourceRange.levelCount     = 1;
        //     barriers[i].subresourceRange.baseArrayLayer = 0;
        //     barriers[i].subresourceRange.layerCount     = 1;
        //     barriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        // }
        // dependency_info.imageMemoryBarrierCount = m_swap_chain_images.size();
        // dependency_info.pImageMemoryBarriers    = barriers.data();
        // vkCmdPipelineBarrier2(command_buffer, &dependency_info);

        // rhi_impl->EndSingleTimeCommands(command_buffer, temp_pool, m_device->GetTransferQueue());

        // LOG_INFO("Vulkan swapchain initialized with {} images.", image_count);
    }
    void VulkanSwapChain::Recreate() {
        LOG_DEBUG("Try to recreate swapchain");
        vkQueueWaitIdle(m_device->GetPresentQueue());
        Cleanup();
        uint32_t width, height;
        Create(&width, &height, false);
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

    VkResult VulkanSwapChain::Present(VkQueue _queue, VkSemaphore _render_finished) {
        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = &_render_finished;
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &m_swap_chain;
        present_info.pImageIndices      = &current_image_index;

        VkResult result = vkQueuePresentKHR(_queue, &present_info);

        current_image_index = (current_image_index + 1) % m_swap_chain_images.size();
        // if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        //     Recreate();

        // } else if (result != VK_SUCCESS) {
        //     assert(false && "Error presenting to swapchain.");
        // }
        return result;
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

        VK_CHECK_RESULT(vkCreateImageView(device->GetDevice(), &create_info, nullptr, &view));

        return view;
    }

    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) {
        SwapChainSupportDetails details;

        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_gpu, _surface, &details.capabilities));

        uint32_t format_count = 0;
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, nullptr));
        if (format_count > 0) {
            details.formats.resize(format_count);
            VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, details.formats.data()));
        }

        uint32_t present_mode_count = 0;
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, nullptr));
        if (present_mode_count > 0) {
            details.present_modes.resize(present_mode_count);
            VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, details.present_modes.data()));
        }

        return details;
    }

    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const Moer::Array<VkSurfaceFormatKHR>& _available_formats, bool _prefer_hdr) {
        for (const auto& format : _available_formats) {
            if (format.format == VK_FORMAT_R8G8B8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        // fallback
        return _available_formats[0];
    }

    VkPresentModeKHR ChooseSwapPresentMode(const Moer::Array<VkPresentModeKHR>& _available_present_modes, bool vsync) {
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

    VkExtent2D ChooseSwapExtent(uint32_t* width, uint32_t* height, const VkSurfaceCapabilitiesKHR& _capabilities) {
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

    VkSwapchain::VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info) : Swapchain(), device(*static_cast<VulkanDevice*>(&_device)) {
        CreateOrRecreate(_info);
    }
    void VkSwapchain::WaitFrameInFlight() {
        if (image_idx < max_frames_in_flight) {
            return;
        }
        auto frame_offset = image_idx % max_frames_in_flight;
        vkWaitForFences(device.GetDevice(), 1, &in_flight_fences[frame_offset], VK_TRUE, UINT64_MAX);
        vkResetFences(device.GetDevice(), 1, &in_flight_fences[frame_offset]);
    }

    VkFence VkSwapchain::GetInFlightFence(uint64 _index) {
        return in_flight_fences[_index % in_flight_fences.size()];
    }

    void VkSwapchain::Recreate(const SwapchainCreateInfo& _info) {
        CreateOrRecreate(_info);
    }
    void VkSwapchain::CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate) {
        bool           b_recreate = handle != VK_NULL_HANDLE || _force_recreate;
        VkSwapchainKHR old_sc     = handle;
        VkInstance     instance   = device.GetInstance();
        //create surface by window handle
        assert(_info.window_handle != 0 && "Window handle is null when creating vulkan swapchain");
        Moer::WindowContext::CreateVulkanSurface(instance, (WindowHandle*)_info.window_handle, VK_NULL_HANDLE, &surface);
        //create swapchain
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.GetGpu(), surface, &capabilities);
        auto details = VkUtil::QuerySwapChainSupport(device.GetGpu(), surface);
        fmt          = ChooseSwapSurfaceFormat(details.formats);
        ChooseSwapExtent(&size.x, &size.y, details.capabilities);
        VkPresentModeKHR present_mode               = ChooseSwapPresentMode(details.present_modes, false);
        format                                      = (EPixelFormat)fmt.format;
        uint                     queue_family_index = device.GetQueueFamilyIndices().graphics.value();
        VkSwapchainCreateInfoKHR create_info{
            .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext                 = nullptr,
            .flags                 = 0,
            .surface               = surface,
            .minImageCount         = capabilities.minImageCount,
            .imageFormat           = fmt.format,
            .imageColorSpace       = fmt.colorSpace,
            .imageExtent           = {size.x, size.y},
            .imageArrayLayers      = 1,
            .imageUsage            = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices   = &queue_family_index,
            .preTransform          = details.capabilities.currentTransform,
            .compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode           = present_mode,
            .clipped               = VK_TRUE,
            .oldSwapchain          = VK_NULL_HANDLE};
        if (old_sc != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device.GetDevice(), old_sc, VK_NULL_HANDLE);
        //create swapchain
        VK_CHECK_RESULT(vkCreateSwapchainKHR(device.GetDevice(), &create_info, nullptr, &handle));

        uint image_cnt;
        vkGetSwapchainImagesKHR(device.GetDevice(), handle, &image_cnt, nullptr);
        max_frames_in_flight       = std::min(max_frames_in_flight, image_cnt);
        bool recreate_fences       = in_flight_fences.size() != max_frames_in_flight;
        bool b_recreate_semaphores = true;
        if (b_recreate) {
            for (size_t i = 0; i < swapchain_textures.size(); ++i) {
                MoerDelete(swapchain_textures[i]);
            }
        }
        swapchain_views.resize(image_cnt);
        swapchain_textures.resize(image_cnt);
        Array<VkImage> images(image_cnt);
        vkGetSwapchainImagesKHR(device.GetDevice(), handle, &image_cnt, images.data());

        for (uint i = 0; i < image_cnt; i++) {
            swapchain_textures[i] = MoerNew(VulkanTexture)(TextureInfo{
                                                               ETextureDimension::TEX_2D,
                                                               ETextureUsageFlags::PRESENT,
                                                               format,
                                                               EClearAttachment{},
                                                               {size.x, size.y, 1},
                                                               1,
                                                               1},
                                                           &device,
                                                           images[i]);
            swapchain_views[i]    = TextureView(swapchain_textures[i]);
        }

        if (b_recreate_semaphores) {
            for (size_t i = 0; i < image_ready_fences.size(); ++i) {
                vkDestroySemaphore(device.GetDevice(), image_ready_fences[i], VK_NULL_HANDLE);
                vkDestroySemaphore(device.GetDevice(), render_finished_fences[i], VK_NULL_HANDLE);
            }
            image_ready_fences.resize(image_cnt);
            render_finished_fences.resize(image_cnt);
            for (uint i = 0; i < image_cnt; i++) {
                VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                vkCreateSemaphore(device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &image_ready_fences[i]);
                vkCreateSemaphore(device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &render_finished_fences[i]);
            }
        }

        if (recreate_fences) {
            for (uint i = 0; i < in_flight_fences.size(); i++) {
                vkDestroyFence(device.GetDevice(), in_flight_fences[i], VK_NULL_HANDLE);
            }
            in_flight_fences.resize(max_frames_in_flight);
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (uint i = 0; i < max_frames_in_flight; i++) {
                vkCreateFence(device.GetDevice(), &fence_info, VK_NULL_HANDLE, &in_flight_fences[i]);
            }
        } else {
            vkResetFences(device.GetDevice(), in_flight_fences.size(), in_flight_fences.data());
        }
        image_idx = 0;
    }
    VkSwapchain::~VkSwapchain() {

        if (handle) {
            vkDestroySwapchainKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
        }
        if (surface) {
            vkDestroySurfaceKHR(device.GetInstance(), surface, VK_NULL_HANDLE);
        }
        for (size_t i = 0; i < swapchain_textures.size(); ++i) {
            MoerDelete(swapchain_textures[i]);
            vkDestroySemaphore(device.GetDevice(), image_ready_fences[i], VK_NULL_HANDLE);
            vkDestroySemaphore(device.GetDevice(), render_finished_fences[i], VK_NULL_HANDLE);
        }
        for (uint i = 0; i < in_flight_fences.size(); i++) {
            vkDestroyFence(device.GetDevice(), in_flight_fences[i], VK_NULL_HANDLE);
        }
    }
    VkSemaphore VkSwapchain::GetImageReadyFence(uint _index) {
        return image_ready_fences[_index % image_ready_fences.size()];
    }
    VkSemaphore VkSwapchain::GetRenderFinishedFence() {
        return render_finished_fences[image_idx % render_finished_fences.size()];
    }
    TextureView VkSwapchain::GetSwapchainImage(uint _index) {
        return swapchain_views[_index % swapchain_views.size()];
    }
    std::tuple<VkSemaphore, uint, uint> VkSwapchain::AquireNextImage() {
        uint32_t    image_index = 0;
        VkSemaphore ready_sem   = image_ready_fences[image_idx % image_ready_fences.size()];

        VkResult result = vkAcquireNextImageKHR(device.GetDevice(), handle, UINT64_MAX, ready_sem, VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            image_index = INT32_MAX;
        }
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            return {ready_sem, image_index, image_idx};
        }
        // assert(false && "Error acquiring next present texture.");
        LOG_WARNING("Fail to acquire next image, window may be resized.");
        return {VK_NULL_HANDLE, INT32_MAX, image_idx};
    }

    void VkSwapchain::Present(VkQueue _queue, uint _index) {
        if (_index == INT32_MAX) {
            return;
        }
        VkSemaphore      finished_semaphores[] = {render_finished_fences[image_idx % image_ready_fences.size()]};
        VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores    = finished_semaphores;
        present_info.swapchainCount     = 1;
        present_info.pSwapchains        = &handle;
        present_info.pImageIndices      = &_index;
        VkResult result                 = vkQueuePresentKHR(_queue, &present_info);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            //recreate manually? because we don't know the window size
        } else if (result != VK_SUCCESS) {
            assert(false && "Error presenting to swapchain.");
        }
        image_idx++;
    }
}// namespace Moer::Render