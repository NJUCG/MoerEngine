//
// Created by 74535 on 2023/10/5.
//

#include "VulkanSwapChain.h"

#include "PixelFormat.h"
#include "VulkanUtil.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"

#include "VulkanPlatform.h"

#include "VulkanCommand.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "vulkan/vulkan_core.h"
#include <atomic>
#include <mutex>
#include <thread>

#include <stdint.h>

namespace VkUtil = Moer::RHI::Vulkan::Util;
namespace Moer::Render {

SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) {
    SwapChainSupportDetails details;

    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_gpu, _surface, &details.capabilities));

    uint32_t format_count = 0;
    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, nullptr));
    if (format_count > 0) {
        details.formats.resize(format_count);
        VK_CHECK_RESULT(
            vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, details.formats.data())
        );
    }

    uint32_t present_mode_count = 0;
    VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, nullptr));
    if (present_mode_count > 0) {
        details.present_modes.resize(present_mode_count);
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(
            _gpu, _surface, &present_mode_count, details.present_modes.data()
        ));
    }

    return details;
}

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(
    const Moer::Array<VkSurfaceFormatKHR>& _available_formats,
    EPixelFormat                           _preferred_format,
    bool                                   _prefer_hdr
) {
    VkFormat preferred_format = VulkanEnumTranslator::METoVKFormat(_preferred_format);

    for (const auto& format : _available_formats) {
        if (format.format == preferred_format && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    // fallback
    return _available_formats[0];
}

VkPresentModeKHR
ChooseSwapPresentMode(const Moer::Array<VkPresentModeKHR>& _available_present_modes, bool vsync) {
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

VkExtent2D
ChooseSwapExtent(uint32_t* width, uint32_t* height, const VkSurfaceCapabilitiesKHR& _capabilities) {
    if (_capabilities.currentExtent.width != static_cast<uint32_t>(-1)) {
        *width  = _capabilities.currentExtent.width;
        *height = _capabilities.currentExtent.height;
        return _capabilities.currentExtent;
    }
    VkExtent2D extent = {static_cast<uint32_t>(*width), static_cast<uint32_t>(*height)};
    extent.width =
        std::clamp(extent.width, _capabilities.minImageExtent.width, _capabilities.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, _capabilities.minImageExtent.height, _capabilities.maxImageExtent.height);

    return extent;
}

VkSwapchain::VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info) :
    Swapchain(),
    device(*static_cast<VulkanDevice*>(&_device)) {
    CreateOrRecreate(_info);
}
void VkSwapchain::WaitFrameInFlight() {
    if (image_idx < max_frames_in_flight) {
        return;
    }
    WaitFrameInFlight(image_idx);
}

void VkSwapchain::WaitFrameInFlight(uint64 _image_idx) {
    // if (_image_idx < max_frames_in_flight) {
    //     return;
    // }
    auto frame_offset = _image_idx % max_frames_in_flight;
    vkWaitForFences(device.GetDevice(), 1, &in_flight_fences[frame_offset], VK_TRUE, UINT64_MAX);
    vkResetFences(device.GetDevice(), 1, &in_flight_fences[frame_offset]);
}

VkFence VkSwapchain::GetInFlightFence(uint64 _index) {
    return in_flight_fences[_index % in_flight_fences.size()];
}

void VkSwapchain::Recreate(const SwapchainCreateInfo& _info) {
    //FIXME: this is not a good way to do it, and it may have issue in multi-threaded env
    //best do sync in a separate thread, and give sync op to user
    // vkQueueWaitIdle(device.GetPresentQueue());
    CreateOrRecreate(_info);
}
void VkSwapchain::CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate) {
    if (_info.size.x == 0 || _info.size.y == 0) {
        size = _info.size;
        LOG_WARNING("Swapchain recreate skipped due to zero window size.");
        return;
    }
    const bool surface_identity_changed =
        surface != VK_NULL_HANDLE &&
        (surface_info.native_window_handle != _info.surface.native_window_handle ||
         surface_info.surface_user_data != _info.surface.surface_user_data ||
         surface_info.create_vulkan_surface != _info.surface.create_vulkan_surface);

    bool           b_recreate = handle != VK_NULL_HANDLE || _force_recreate;
    VkSwapchainKHR old_sc     = handle;
    VkInstance     instance   = device.GetInstance();
    if (b_recreate) {
        Sync();
    }

    if (surface_identity_changed) {
        if (old_sc != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device.GetDevice(), old_sc, VK_NULL_HANDLE);
            old_sc = VK_NULL_HANDLE;
            handle = VK_NULL_HANDLE;
        }
        vkDestroySurfaceKHR(device.GetInstance(), surface, VK_NULL_HANDLE);
        surface = VK_NULL_HANDLE;
    }

    if (surface == VK_NULL_HANDLE) {
        assert(_info.surface.create_vulkan_surface && "Vulkan swapchain requires a surface creation callback");
        _info.surface.create_vulkan_surface(
            _info.surface.surface_user_data,
            instance,
            VK_NULL_HANDLE,
            &surface
        );
        assert(surface != VK_NULL_HANDLE && "Vulkan surface creation returned a null surface");
        surface_info = _info.surface;
    }
    //create swapchain
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.GetGpu(), surface, &capabilities);
    auto details = VkUtil::QuerySwapChainSupport(device.GetGpu(), surface);
    fmt          = ChooseSwapSurfaceFormat(details.formats, _info.preferred_format);
    size         = _info.size;
    ChooseSwapExtent(&size.x, &size.y, details.capabilities);
    if (size.x == 0 || size.y == 0) {
        LOG_WARNING("Swapchain recreate skipped due to zero swapchain extent.");
        return;
    }
    VkPresentModeKHR present_mode = ChooseSwapPresentMode(details.present_modes, false);
    format                        = (EPixelFormat)fmt.format;
    uint     queue_family_index   = device.GetQueueFamilyIndices().graphics.value();
    uint32_t image_count          = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    VkSwapchainCreateInfoKHR create_info{
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext                 = nullptr,
        .flags                 = 0,
        .surface               = surface,
        .minImageCount         = image_count,
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
        .oldSwapchain          = old_sc
    };
    //create swapchain
    VK_CHECK_RESULT(vkCreateSwapchainKHR(device.GetDevice(), &create_info, nullptr, &handle));
    if (old_sc != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device.GetDevice(), old_sc, VK_NULL_HANDLE);
    }

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
        swapchain_textures[i] = MoerNew(VulkanTexture)(
            TextureInfo{
                ETextureDimension::TEX_2D,
                ETextureUsageFlags::PRESENT,
                format,
                EClearAttachment{},
                {size.x, size.y, 1}
            },
            &device,
            images[i]
        );
        swapchain_views[i] = TextureView(swapchain_textures[i]);
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
            device.SetResourceName(
                uint64(image_ready_fences[i]),
                VK_OBJECT_TYPE_SEMAPHORE,
                "ImageReadySemaphore_" + std::to_string(i)
            );
            vkCreateSemaphore(
                device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &render_finished_fences[i]
            );
            device.SetResourceName(
                uint64(render_finished_fences[i]),
                VK_OBJECT_TYPE_SEMAPHORE,
                "RenderFinishedSemaphore_" + std::to_string(i)
            );
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
            device.SetResourceName(
                uint64(in_flight_fences[i]), VK_OBJECT_TYPE_FENCE, "InFlightFence_" + std::to_string(i)
            );
        }
    } else if (!in_flight_fences.empty()) {
        VK_CHECK_RESULT(vkResetFences(
            device.GetDevice(),
            static_cast<uint32_t>(in_flight_fences.size()),
            in_flight_fences.data()
        ));
    }
    image_idx = 0;
}
VkSwapchain::~VkSwapchain() {
    if (handle != VK_NULL_HANDLE) {
        Sync();
    }

    if (handle) {
        vkDestroySwapchainKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
        handle = VK_NULL_HANDLE;
    }
    if (surface) {
        vkDestroySurfaceKHR(device.GetInstance(), surface, VK_NULL_HANDLE);
        surface = VK_NULL_HANDLE;
    }
    for (size_t i = 0; i < swapchain_textures.size(); ++i) {
        MoerDelete(swapchain_textures[i]);
        swapchain_textures[i] = nullptr;
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
    uint32_t    aquire_idx = 0;
    VkSemaphore ready_sem  = image_ready_fences[image_idx % image_ready_fences.size()];

    VkResult result =
        vkAcquireNextImageKHR(device.GetDevice(), handle, UINT64_MAX, ready_sem, VK_NULL_HANDLE, &aquire_idx);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        aquire_idx = UINT32_MAX;
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        return {ready_sem, aquire_idx, image_idx};
    }
    // assert(false && "Error acquiring next present texture.");
    LOG_WARNING("Fail to acquire next image, window may be resized.");
    return {VK_NULL_HANDLE, UINT32_MAX, image_idx};
}

void VkSwapchain::Present(VkQueue _queue, uint _index) {
    if (_index == UINT32_MAX) {
        return;
    }
    VkSemaphore finished_semaphores[] = {render_finished_fences[image_idx % render_finished_fences.size()]};
    // 如果启用了 VK_EXT_swapchain_maintenance1，则使用 present fence 优化队列同步；
    // 否则退回到兼容路径，不挂 VkSwapchainPresentFenceInfoEXT，避免验证层报扩展未启用。
    const bool use_present_fence =
        device.GetOptionalExtensions().m_has_khr_swapchain_maintenance1;

    VkSwapchainPresentFenceInfoEXT present_fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (use_present_fence) {
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences        = &in_flight_fences[image_idx % in_flight_fences.size()];
    }

    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.pNext              = use_present_fence ? reinterpret_cast<void*>(&present_fence_info) : nullptr;
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
    ++image_idx;
}

void VkSwapchain::Sync() {
    vkQueueWaitIdle(device.GetPresentQueue());
}
} // namespace Moer::Render
