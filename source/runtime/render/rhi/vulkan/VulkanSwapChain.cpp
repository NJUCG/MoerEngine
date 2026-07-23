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
#include "window/WindowContext.h"
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
    present_threads.resize(max_frames_in_flight);
    CreateOrRecreate(_info);
}
bool VkSwapchain::WaitFrameInFlight() {
    // if (image_idx < max_frames_in_flight) {
    //     return;
    // }
    // auto frame_offset = image_idx % max_frames_in_flight;
    // vkWaitForFences(device.GetDevice(), 1, &in_flight_fences[frame_offset], VK_TRUE, UINT64_MAX);
    // vkResetFences(device.GetDevice(), 1, &in_flight_fences[frame_offset]);
    while (!device.IsFaulted() &&
           cur_present_cnt.load(std::memory_order_relaxed) >= max_frames_in_flight) {
        std::this_thread::yield();
    }
    return !device.IsFaulted();
}

bool VkSwapchain::WaitFrameInFlight(uint64 _image_idx) {
    // if (_image_idx < max_frames_in_flight) {
    //     return;
    // }
    auto frame_offset = _image_idx % max_frames_in_flight;
    while (!device.IsFaulted()) {
        const VulkanOperationContext context{
            .operation  = EVulkanFaultOperation::PresentFenceWait,
            .queue_type = EQueueType::Graphics,
            .queue      = device.GetPresentQueue(),
            .timeline   = _image_idx,
        };
        const VkResult result = vkWaitForFences(
            device.GetDevice(), 1, &in_flight_fences[frame_offset], VK_TRUE, 50'000'000
        );
        if (result == VK_TIMEOUT) {
            continue;
        }
        if (result != VK_SUCCESS) {
            device.TryLatchFirstFault(context, result);
            if (result != VK_ERROR_DEVICE_LOST) {
                device.EmergencyExitWithoutVulkanCleanup(context, result);
            }
            return false;
        }
        if (device.IsDeviceLost()) {
            return false;
        }
        const VkResult reset_result = device.ResetFence(
            in_flight_fences[frame_offset],
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::PresentFenceReset,
                .queue_type = EQueueType::Graphics,
                .queue      = device.GetPresentQueue(),
                .timeline   = _image_idx,
            }
        );
        return reset_result == VK_SUCCESS && !device.IsFaulted();
    }
    return false;
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
    (void)_force_recreate;
    if (device.IsFaulted()) {
        return;
    }
    if (_info.size.x == 0 || _info.size.y == 0) {
        LOG_WARNING("Swapchain recreate skipped due to zero window size.");
        return;
    }

    Sync();
    if (device.IsFaulted()) {
        return;
    }

    const VkSwapchainKHR old_sc     = handle;
    VkInstance           instance   = device.GetInstance();
    //create surface by window handle
    assert(_info.window_handle != 0 && "Window handle is null when creating vulkan swapchain");
    if (surface == VK_NULL_HANDLE) {
        Moer::WindowContext::CreateVulkanSurface(
            instance, (WindowHandle*)_info.window_handle, VK_NULL_HANDLE, &surface
        );
    }
    //create swapchain
    const auto         details = VkUtil::QuerySwapChainSupport(device.GetGpu(), surface);
    VkSurfaceFormatKHR new_fmt = ChooseSwapSurfaceFormat(details.formats, _info.preferred_format);
    Extent2D           new_size{_info.size.x, _info.size.y};
    ChooseSwapExtent(&new_size.x, &new_size.y, details.capabilities);
    if (new_size.x == 0 || new_size.y == 0) {
        LOG_WARNING("Swapchain recreate skipped due to zero swapchain extent.");
        return;
    }
    VkPresentModeKHR present_mode = ChooseSwapPresentMode(details.present_modes, false);
    const EPixelFormat new_format = (EPixelFormat)new_fmt.format;
    uint     queue_family_index   = device.GetQueueFamilyIndices().graphics.value();
    uint32_t image_count          = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && image_count > details.capabilities.maxImageCount) {
        image_count = details.capabilities.maxImageCount;
    }
    VkSwapchainCreateInfoKHR create_info{
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext                 = nullptr,
        .flags                 = 0,
        .surface               = surface,
        .minImageCount         = image_count,
        .imageFormat           = new_fmt.format,
        .imageColorSpace       = new_fmt.colorSpace,
        .imageExtent           = {new_size.x, new_size.y},
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

    auto fault_admission = device.AcquireFaultAdmission();
    if (device.IsFaulted()) {
        return;
    }

    VkSwapchainKHR          new_sc = VK_NULL_HANDLE;
    Array<VkSemaphore>      new_image_ready_fences;
    Array<VkSemaphore>      new_render_finished_fences;
    Array<VkFence>          new_in_flight_fences;
    Array<VulkanTexture*>   new_swapchain_textures;
    Array<TextureView>      new_swapchain_views;

    auto cleanup_new_resources = [&]() {
        for (auto* texture : new_swapchain_textures) {
            if (texture != nullptr) {
                MoerDelete(texture);
            }
        }
        for (VkSemaphore semaphore : new_image_ready_fences) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device.GetDevice(), semaphore, VK_NULL_HANDLE);
            }
        }
        for (VkSemaphore semaphore : new_render_finished_fences) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device.GetDevice(), semaphore, VK_NULL_HANDLE);
            }
        }
        for (VkFence fence : new_in_flight_fences) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(device.GetDevice(), fence, VK_NULL_HANDLE);
            }
        }
        if (new_sc != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device.GetDevice(), new_sc, VK_NULL_HANDLE);
            new_sc = VK_NULL_HANDLE;
        }
    };
    auto retire_old_resources = [&]() {
        if (old_sc == VK_NULL_HANDLE) {
            return;
        }
        for (auto* texture : swapchain_textures) {
            MoerDelete(texture);
        }
        swapchain_textures.clear();
        swapchain_views.clear();
        for (VkSemaphore semaphore : image_ready_fences) {
            vkDestroySemaphore(device.GetDevice(), semaphore, VK_NULL_HANDLE);
        }
        image_ready_fences.clear();
        for (VkSemaphore semaphore : render_finished_fences) {
            vkDestroySemaphore(device.GetDevice(), semaphore, VK_NULL_HANDLE);
        }
        render_finished_fences.clear();
        vkDestroySwapchainKHR(device.GetDevice(), old_sc, VK_NULL_HANDLE);
        handle = VK_NULL_HANDLE;
    };
    auto check_device_result = [&](VkResult _result, EVulkanFaultOperation _operation) {
        if (_result == VK_SUCCESS && !device.IsFaulted()) {
            return true;
        }
        if (fault_admission.owns_lock()) {
            fault_admission.unlock();
        }
        if (_result != VK_SUCCESS) {
            device.TryLatchFirstFault(
                VulkanOperationContext{.operation = _operation}, _result, false, false, true
            );
        }
        return false;
    };
    auto abort_transaction = [&]() {
        cleanup_new_resources();
    };

    VkResult result = vkCreateSwapchainKHR(device.GetDevice(), &create_info, nullptr, &new_sc);
    // Passing a non-null oldSwapchain retires it even when creation fails.
    retire_old_resources();
    if (result != VK_SUCCESS) {
        new_sc = VK_NULL_HANDLE;
    }
    if (!check_device_result(result, EVulkanFaultOperation::SwapchainCreate)) {
        abort_transaction();
        return;
    }

    uint32_t       image_cnt = 0;
    Array<VkImage> images;
    bool           images_ready = false;
    for (uint attempt = 0; attempt < 4; ++attempt) {
        result = vkGetSwapchainImagesKHR(device.GetDevice(), new_sc, &image_cnt, nullptr);
        if (!check_device_result(result, EVulkanFaultOperation::SwapchainGetImages)) {
            abort_transaction();
            return;
        }
        if (image_cnt == 0) {
            check_device_result(
                VK_ERROR_INITIALIZATION_FAILED, EVulkanFaultOperation::SwapchainGetImages
            );
            abort_transaction();
            return;
        }

        images.assign(image_cnt, VK_NULL_HANDLE);
        uint32_t fetched_image_cnt = image_cnt;
        result = vkGetSwapchainImagesKHR(
            device.GetDevice(), new_sc, &fetched_image_cnt, images.data()
        );
        if (result == VK_INCOMPLETE) {
            continue;
        }
        if (!check_device_result(result, EVulkanFaultOperation::SwapchainGetImages)) {
            abort_transaction();
            return;
        }
        if (fetched_image_cnt == 0) {
            check_device_result(
                VK_ERROR_INITIALIZATION_FAILED, EVulkanFaultOperation::SwapchainGetImages
            );
            abort_transaction();
            return;
        }
        image_cnt = fetched_image_cnt;
        images.resize(image_cnt);
        images_ready = true;
        break;
    }
    if (!images_ready) {
        LOG_ERROR("Swapchain image enumeration remained incomplete after four attempts.");
        check_device_result(
            VK_ERROR_INITIALIZATION_FAILED, EVulkanFaultOperation::SwapchainGetImages
        );
        abort_transaction();
        return;
    }

    const uint new_max_frames_in_flight = std::min(max_frames_in_flight, uint(image_cnt));
    const bool recreate_fences = in_flight_fences.size() != new_max_frames_in_flight;
    new_image_ready_fences.resize(image_cnt, VK_NULL_HANDLE);
    new_render_finished_fences.resize(image_cnt, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint i = 0; i < image_cnt; i++) {
        result = vkCreateSemaphore(
            device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &new_image_ready_fences[i]
        );
        if (result != VK_SUCCESS) {
            new_image_ready_fences[i] = VK_NULL_HANDLE;
        }
        if (!check_device_result(result, EVulkanFaultOperation::SwapchainSemaphoreCreate)) {
            abort_transaction();
            return;
        }
        device.SetResourceName(
            uint64(new_image_ready_fences[i]),
            VK_OBJECT_TYPE_SEMAPHORE,
            "ImageReadySemaphore_" + std::to_string(i)
        );

        result = vkCreateSemaphore(
            device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &new_render_finished_fences[i]
        );
        if (result != VK_SUCCESS) {
            new_render_finished_fences[i] = VK_NULL_HANDLE;
        }
        if (!check_device_result(result, EVulkanFaultOperation::SwapchainSemaphoreCreate)) {
            abort_transaction();
            return;
        }
        device.SetResourceName(
            uint64(new_render_finished_fences[i]),
            VK_OBJECT_TYPE_SEMAPHORE,
            "RenderFinishedSemaphore_" + std::to_string(i)
        );
    }

    if (recreate_fences) {
        new_in_flight_fences.resize(new_max_frames_in_flight, VK_NULL_HANDLE);
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint i = 0; i < new_max_frames_in_flight; i++) {
            result = vkCreateFence(
                device.GetDevice(), &fence_info, VK_NULL_HANDLE, &new_in_flight_fences[i]
            );
            if (result != VK_SUCCESS) {
                new_in_flight_fences[i] = VK_NULL_HANDLE;
            }
            if (!check_device_result(result, EVulkanFaultOperation::SwapchainFenceCreate)) {
                abort_transaction();
                return;
            }
            device.SetResourceName(
                uint64(new_in_flight_fences[i]),
                VK_OBJECT_TYPE_FENCE,
                "InFlightFence_" + std::to_string(i)
            );
        }
    }

    new_swapchain_textures.resize(image_cnt, nullptr);
    new_swapchain_views.resize(image_cnt);
    for (uint i = 0; i < image_cnt; i++) {
        new_swapchain_textures[i] = MoerNew(VulkanTexture)(
            TextureInfo{
                ETextureDimension::TEX_2D,
                ETextureUsageFlags::PRESENT,
                new_format,
                EClearAttachment{},
                {new_size.x, new_size.y, 1}
            },
            &device,
            images[i]
        );
        new_swapchain_views[i] = TextureView(new_swapchain_textures[i]);
    }
    if (!check_device_result(VK_SUCCESS, EVulkanFaultOperation::SwapchainCreate)) {
        abort_transaction();
        return;
    }

    if (recreate_fences) {
        for (VkFence fence : in_flight_fences) {
            vkDestroyFence(device.GetDevice(), fence, VK_NULL_HANDLE);
        }
    }

    handle                    = new_sc;
    new_sc                    = VK_NULL_HANDLE;
    fmt                       = new_fmt;
    size                      = new_size;
    format                    = new_format;
    max_frames_in_flight      = new_max_frames_in_flight;
    image_ready_fences        = std::move(new_image_ready_fences);
    render_finished_fences    = std::move(new_render_finished_fences);
    swapchain_textures        = std::move(new_swapchain_textures);
    swapchain_views           = std::move(new_swapchain_views);
    if (recreate_fences) {
        in_flight_fences = std::move(new_in_flight_fences);
    }
    image_idx = 0;
}
VkSwapchain::~VkSwapchain() {
    Sync();

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
VkSemaphore VkSwapchain::GetRenderFinishedFence(uint _image_index) {
    return render_finished_fences[_image_index % render_finished_fences.size()];
}
TextureView VkSwapchain::GetSwapchainImage(uint _index) {
    return swapchain_views[_index % swapchain_views.size()];
}
VkSwapchain::AcquireResult VkSwapchain::AquireNextImage(uint64 _timeout) {
    uint32_t    aquire_idx = UINT32_MAX;
    VkSemaphore ready_sem  = image_ready_fences[image_idx % image_ready_fences.size()];

    const VulkanOperationResult outcome = device.AcquireNextImage(
        handle,
        _timeout,
        ready_sem,
        VK_NULL_HANDLE,
        &aquire_idx,
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::AcquireNextImage,
            .queue_type = EQueueType::Graphics,
            .queue      = device.GetGraphicsQueue(),
            .timeline   = image_idx,
        }
    );
    if ((outcome.result == VK_SUCCESS || outcome.result == VK_SUBOPTIMAL_KHR) &&
        aquire_idx != UINT32_MAX) {
        return {outcome, ready_sem, aquire_idx, image_idx};
    }
    return {outcome, VK_NULL_HANDLE, UINT32_MAX, image_idx};
}

VulkanOperationResult VkSwapchain::Present(
    VkQueue _queue,
    uint    _index,
    uint64  _timeline,
    uint64  _work_serial
) {
    if (_index == UINT32_MAX) {
        return {EVulkanOperationStatus::Retry, VK_NOT_READY};
    }
    VkSemaphore finished_semaphores[] = {GetRenderFinishedFence(_index)};
    // 如果启用了 VK_EXT_swapchain_maintenance1，则使用 present fence 优化队列同步；
    // 否则退回到兼容路径，不挂 VkSwapchainPresentFenceInfoEXT，避免验证层报扩展未启用。
    const bool use_present_fence =
        device.HasDeviceExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    if (use_present_fence && !PreparePresentFence(image_idx)) {
        return {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
    }

    VkSwapchainPresentFenceInfoEXT present_fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (use_present_fence) {
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences        = &in_flight_fences[image_idx % in_flight_fences.size()];
    }

    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.pNext = use_present_fence ? reinterpret_cast<void*>(&present_fence_info) : nullptr;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = finished_semaphores;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &handle;
    present_info.pImageIndices      = &_index;
    const VulkanOperationResult outcome = device.PresentOnQueue(
        _queue,
        present_info,
        VulkanOperationContext{
            .operation   = EVulkanFaultOperation::QueuePresent,
            .queue_type  = EQueueType::Graphics,
            .queue       = _queue,
            .timeline    = _timeline,
            .work_serial = _work_serial,
        }
    );
    if (use_present_fence &&
        (outcome.status == EVulkanOperationStatus::Success ||
         outcome.status == EVulkanOperationStatus::Recreate)) {
        // 仅在使用 present fence 的情况下异步等待 in_flight_fences
        EnqueuePresent(image_idx);
    }
    ++image_idx;
    return outcome;
}

void VkSwapchain::OnFinishPresent(uint64 _image_idx) {
    cur_present_cnt.fetch_sub(1, std::memory_order_acq_rel);
}

bool VkSwapchain::PreparePresentFence(uint64 _present_idx) {
    auto& thread = present_threads[_present_idx % max_frames_in_flight];
    if (thread.joinable()) {
        thread.join();
    }
    return !device.IsFaulted();
}

void VkSwapchain::EnqueuePresent(uint64 _present_idx) {

    cur_present_cnt.fetch_add(1, std::memory_order_acq_rel);
    auto& thread = present_threads[_present_idx % max_frames_in_flight];
    assert(!thread.joinable() && "Present fence slot must be retired before reuse");
    thread = std::jthread([this, _present_idx]() {
        const uint64 frame_index = _present_idx % max_frames_in_flight;
        while (!device.IsDeviceLost()) {
            const VulkanOperationContext wait_context{
                .operation  = EVulkanFaultOperation::PresentFenceWait,
                .queue_type = EQueueType::Graphics,
                .queue      = device.GetPresentQueue(),
                .timeline   = _present_idx,
            };
            const VkResult result = vkWaitForFences(
                device.GetDevice(), 1, &in_flight_fences[frame_index], VK_TRUE, 50'000'000
            );
            if (result == VK_TIMEOUT) {
                continue;
            }
            if (result == VK_SUCCESS && !device.IsDeviceLost()) {
                const VkResult reset_result = device.ResetFence(
                    in_flight_fences[frame_index],
                    VulkanOperationContext{
                        .operation  = EVulkanFaultOperation::PresentFenceReset,
                        .queue_type = EQueueType::Graphics,
                        .queue      = device.GetPresentQueue(),
                        .timeline   = _present_idx,
                    }
                );
                if (reset_result != VK_SUCCESS) {
                    break;
                }
            } else if (result != VK_SUCCESS) {
                device.TryLatchFirstFault(wait_context, result);
                if (result != VK_ERROR_DEVICE_LOST) {
                    device.EmergencyExitWithoutVulkanCleanup(wait_context, result);
                }
            }
            break;
        }
        OnFinishPresent(_present_idx);
    });
}

void VkSwapchain::Sync() {
    //wait for present copy
    while (!device.IsDeviceLost() && cur_present_cnt.load(std::memory_order_relaxed) > 0) {
        std::this_thread::yield();
    }
    for (auto& thread : present_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    if (device.IsDeviceLost()) {
        return;
    }
    VkQueue graphics_queue = device.GetGraphicsQueue();
    VkQueue present_queue  = device.GetPresentQueue();
    VkResult result        = device.WaitQueueIdle(
        graphics_queue,
        VulkanOperationContext{
            .operation  = EVulkanFaultOperation::QueueWaitIdle,
            .queue_type = EQueueType::Graphics,
            .queue      = graphics_queue,
        }
    );
    if (result != VK_SUCCESS && device.IsDeviceLost()) {
        return;
    }
    if (present_queue != graphics_queue) {
        (void)device.WaitQueueIdle(
            present_queue,
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::QueueWaitIdle,
                .queue_type = EQueueType::Graphics,
                .queue      = present_queue,
            }
        );
    }
}
} // namespace Moer::Render
