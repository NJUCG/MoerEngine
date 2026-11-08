#include "VulkanSwapChain.h"
#include "VulkanEnumTranslator.h"

#include "PixelFormat.h"
#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "VulkanSurfaceSupport.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"
#include "misc/RAII.h"
#include "rhi/RHICommon.h"

#include "misc/Types.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cassert>
#include <string>
#include <utility>

namespace Moer::Render {

namespace {

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    constexpr VkCompositeAlphaFlagBitsKHR preferred_modes[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const VkCompositeAlphaFlagBitsKHR mode : preferred_modes) {
        if ((capabilities.supportedCompositeAlpha & mode) != 0) {
            return mode;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

[[nodiscard]] bool IsDeviceWideTerminalSwapchainResult(VkResult result) noexcept {
    return result == VK_ERROR_DEVICE_LOST || result == VK_ERROR_OUT_OF_HOST_MEMORY ||
           result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

[[nodiscard]] bool IsPresentationLocalSwapchainResult(VkResult result) noexcept {
    // These failures invalidate or temporarily block presentation state, not
    // the logical device. Keep recovery pending so a later frame can retry.
    return result == VK_SUBOPTIMAL_KHR || result == VK_INCOMPLETE ||
           result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR ||
           result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR ||
           result == VK_ERROR_INCOMPATIBLE_DISPLAY_KHR ||
           result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
           result == VK_ERROR_FORMAT_NOT_SUPPORTED;
}

[[nodiscard]] bool ShouldLatchSwapchainFailure(
    bool                  force_surface_recreate,
    EVulkanFaultOperation operation,
    VkResult              result
) noexcept {
    if (IsPresentationLocalSwapchainResult(result)) {
        return false;
    }
    if (!force_surface_recreate || IsDeviceWideTerminalSwapchainResult(result)) {
        return true;
    }
    switch (operation) {
        case EVulkanFaultOperation::SwapchainSurfaceCreate:
        case EVulkanFaultOperation::SwapchainSurfaceQuery:
        case EVulkanFaultOperation::SwapchainCreate:
        case EVulkanFaultOperation::SwapchainGetImages:
            return false;
        default:
            return true;
    }
}

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(
    const Moer::Array<VkSurfaceFormatKHR>& _available_formats,
    EPixelFormat                           _preferred_format
) {
    VkFormat preferred_format = VulkanEnumTranslator::METoVKFormat(_preferred_format);

    if (_available_formats.size() == 1 && _available_formats[0].format == VK_FORMAT_UNDEFINED) {
        return {
            .format     = preferred_format,
            .colorSpace = _available_formats[0].colorSpace,
        };
    }
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

VkExtent2D ChooseSwapExtent(VkExtent2D _requested_extent, const VkSurfaceCapabilitiesKHR& _capabilities) {
    if (_capabilities.currentExtent.width != UINT32_MAX) {
        return _capabilities.currentExtent;
    }
    VkExtent2D extent = _requested_extent;
    extent.width =
        std::clamp(extent.width, _capabilities.minImageExtent.width, _capabilities.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, _capabilities.minImageExtent.height, _capabilities.maxImageExtent.height);

    return extent;
}

VkResult CreateWindowSurface(VkInstance instance, const WindowNativeHandle& native_window, VkSurfaceKHR& surface) {
    if (!native_window.IsValid() || native_window.window_system != EWindowSystemType::GLFW || instance == VK_NULL_HANDLE) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return glfwCreateWindowSurface(
        instance, reinterpret_cast<GLFWwindow*>(native_window.window_system_handle), nullptr, &surface
    );
}

VkSwapchainCreateInfoKHR MakeSwapchainCreateInfo(
    const SwapChainSupportDetails& details,
    const SwapchainCreateInfo&     request,
    VkSurfaceKHR                   surface,
    VkSwapchainKHR                 old_swapchain,
    const uint32 (&queue_families)[2]
) {
    const auto format      = ChooseSwapSurfaceFormat(details.formats, request.preferred_format);
    uint32     image_count = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, details.capabilities.maxImageCount);
    }
    const bool concurrent = queue_families[0] != queue_families[1];
    return {
        .sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface               = surface,
        .minImageCount         = image_count,
        .imageFormat           = format.format,
        .imageColorSpace       = format.colorSpace,
        .imageExtent           = ChooseSwapExtent({request.size.x, request.size.y}, details.capabilities),
        .imageArrayLayers      = 1,
        .imageUsage            = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode      = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = concurrent ? 2u : 0u,
        .pQueueFamilyIndices   = concurrent ? queue_families : nullptr,
        .preTransform          = details.capabilities.currentTransform,
        .compositeAlpha        = ChooseCompositeAlpha(details.capabilities),
        .presentMode           = ChooseSwapPresentMode(details.present_modes, false),
        .clipped               = VK_TRUE,
        .oldSwapchain          = old_swapchain
    };
}

} // namespace

VkSwapchain::VkSwapchain(VulkanDevice& _device, const SwapchainCreateInfo& _info) :
    Swapchain(),
    device(_device) {
    static_cast<void>(CreateOrRecreate(_info));
}

bool VkSwapchain::Recreate(const SwapchainCreateInfo& _info) {
    return CreateOrRecreate(_info, _info.force_surface_recreate);
}
bool VkSwapchain::CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate) {
    if (device.IsFaulted()) {
        return false;
    }
    native_surface_recreate_pending_ =
        native_surface_recreate_pending_ || _force_recreate;
    const bool effective_force_surface_recreate =
        native_surface_recreate_pending_;
    if (_info.size.x == 0 || _info.size.y == 0) {
        LOG_WARNING("Swapchain recreate skipped due to zero window size.");
        return false;
    }

    const auto observe_setup_result = [this](VkResult result) {
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            native_surface_recreate_pending_ = true;
        }
    };

    ESwapchainSurfaceTransition surface_transition =
        ClassifySwapchainSurfaceTransition(surface_info, surface != VK_NULL_HANDLE, _info.surface);
    if (effective_force_surface_recreate &&
        surface_transition == ESwapchainSurfaceTransition::Reuse) {
        // Surface loss invalidates the native VkSurfaceKHR without changing
        // the window-system identity. Force the destructive recovery path,
        // which keeps oldSwapchain null for the replacement surface.
        surface_transition = ESwapchainSurfaceTransition::Replace;
    }
    if (surface_transition == ESwapchainSurfaceTransition::Invalid) {
        LOG_ERROR("Vulkan swapchain creation requires a valid window surface source.");
        if (ShouldLatchSwapchainFailure(
                effective_force_surface_recreate,
                EVulkanFaultOperation::SwapchainSurfaceCreate,
                VK_ERROR_INITIALIZATION_FAILED
            )) {
            device.TryLatchFirstFault(
                VulkanOperationContext{.operation = EVulkanFaultOperation::SwapchainSurfaceCreate},
                VK_ERROR_INITIALIZATION_FAILED,
                false,
                false,
                true
            );
        }
        return false;
    }

    // PresentationSurface drains ordinary submissions and waits for device
    // idle before replacing native swapchain resources.
    if (device.IsFaulted()) {
        return false;
    }

    if (effective_force_surface_recreate) {
        // A lost native surface is no longer a usable transactional fallback.
        // Some window systems also reject a second surface for the same
        // native window while the old one exists, so retire in Vulkan order
        // before asking the platform to create its replacement.
        DestroyImages(swapchain_images);
        if (handle != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(
                device.GetDevice(), handle, VK_NULL_HANDLE
            );
            handle = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(
                device.GetInstance(), surface, VK_NULL_HANDLE
            );
            surface = VK_NULL_HANDLE;
        }
        surface_info = {};
    }

    VkSurfaceKHR candidate_surface         = surface;
    const bool   candidate_owns_surface    = surface_transition != ESwapchainSurfaceTransition::Reuse;
    const auto   cleanup_candidate_surface = OnScopeExit([&] {
        if (candidate_owns_surface && candidate_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(device.GetInstance(), candidate_surface, VK_NULL_HANDLE);
        }
    });
    if (candidate_owns_surface) {
        candidate_surface                      = VK_NULL_HANDLE;
        const WindowNativeHandle native_window = _info.surface.source->GetNativeWindow();
        VkResult                 native_result = CreateWindowSurface(device.GetInstance(), native_window, candidate_surface);
        if (native_result != VK_SUCCESS || candidate_surface == VK_NULL_HANDLE) {
            if (native_result == VK_SUCCESS) {
                native_result = VK_ERROR_INITIALIZATION_FAILED;
            }
            observe_setup_result(native_result);
            LOG_ERROR(
                "Window surface creation failed: window_system={}, native_result={}.",
                static_cast<uint32>(native_window.window_system),
                static_cast<int32>(native_result)
            );
            if (ShouldLatchSwapchainFailure(
                    effective_force_surface_recreate,
                    EVulkanFaultOperation::SwapchainSurfaceCreate,
                    native_result
                )) {
                device.TryLatchFirstFault(
                    VulkanOperationContext{.operation = EVulkanFaultOperation::SwapchainSurfaceCreate},
                    native_result,
                    false,
                    false,
                    true
                );
            }
            return false;
        }
    }

    const QueueFamilyIndices queue_families = device.GetQueueFamilyIndices();
    if (!queue_families.graphics.has_value() || !queue_families.present.has_value()) {
        LOG_ERROR("Vulkan swapchain creation requires initialized graphics and present queue families.");
        device.TryLatchFirstFault(
            VulkanOperationContext{.operation = EVulkanFaultOperation::SwapchainSurfaceQuery},
            VK_ERROR_INITIALIZATION_FAILED,
            false,
            false,
            true
        );
        return false;
    }

    const SwapChainSupportQueryResult support_query =
        QuerySwapChainSupport(device.GetGpu(), candidate_surface, queue_families.present.value());
    if (!support_query.Succeeded()) {
        observe_setup_result(support_query.native_result);
        LOG_ERROR(
            "Swapchain surface query failed: status={}, native_result={}, present_queue_family={}.",
            static_cast<uint32>(support_query.status),
            static_cast<int32>(support_query.native_result),
            queue_families.present.value()
        );
        if (ShouldLatchSwapchainFailure(
                effective_force_surface_recreate,
                EVulkanFaultOperation::SwapchainSurfaceQuery,
                support_query.native_result
            )) {
            device.TryLatchFirstFault(
                VulkanOperationContext{.operation = EVulkanFaultOperation::SwapchainSurfaceQuery},
                support_query.native_result,
                false,
                false,
                true
            );
        }
        return false;
    }

    const uint32 queue_family_indices[] = {
        queue_families.graphics.value(), queue_families.present.value()
    };
    const auto create_info = MakeSwapchainCreateInfo(
        support_query.details, _info, candidate_surface, surface_transition == ESwapchainSurfaceTransition::Reuse ? handle : VK_NULL_HANDLE, queue_family_indices
    );
    if (create_info.imageExtent.width == 0 || create_info.imageExtent.height == 0) {
        LOG_WARNING("Swapchain recreate skipped due to zero swapchain extent.");
        return false;
    }
    const auto new_format = VulkanEnumTranslator::VKToMEFormat(create_info.imageFormat);
    if (new_format == PF_UNDEFINED && create_info.imageFormat != VK_FORMAT_UNDEFINED) {
        LOG_ERROR("Swapchain selected an unmapped Vulkan format: {}", static_cast<uint32>(create_info.imageFormat));
        return false;
    }

    auto fault_admission = device.AcquireFaultAdmission();
    if (device.IsFaulted()) {
        return false;
    }

    VkSwapchainKHR        new_sc = VK_NULL_HANDLE;
    Array<SwapchainImage> new_swapchain_images;

    const auto cleanup_new_resources = OnScopeExit([&] {
        DestroyImages(new_swapchain_images);
        if (new_sc != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device.GetDevice(), new_sc, VK_NULL_HANDLE);
        }
    });
    const auto retire_old_resources  = [&] {
        if (handle != VK_NULL_HANDLE) {
            DestroyImages(swapchain_images);
            vkDestroySwapchainKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
            handle = VK_NULL_HANDLE;
        }
    };
    auto check_device_result = [&](VkResult _result, EVulkanFaultOperation _operation) {
        observe_setup_result(_result);
        if (_result == VK_SUCCESS && !device.IsFaulted()) {
            return true;
        }
        if (fault_admission.owns_lock()) {
            fault_admission.unlock();
        }
        if (_result != VK_SUCCESS) {
            if (ShouldLatchSwapchainFailure(
                    effective_force_surface_recreate,
                    _operation,
                    _result
                )) {
                device.TryLatchFirstFault(
                    VulkanOperationContext{.operation = _operation}, _result, false, false, true
                );
            }
        }
        return false;
    };

    const auto result = vkCreateSwapchainKHR(device.GetDevice(), &create_info, nullptr, &new_sc);
    // Passing a non-null oldSwapchain retires it even when creation fails.
    if (create_info.oldSwapchain != VK_NULL_HANDLE) {
        retire_old_resources();
    }
    if (result != VK_SUCCESS) {
        new_sc = VK_NULL_HANDLE;
    }
    if (!check_device_result(result, EVulkanFaultOperation::SwapchainCreate)) {
        return false;
    }

    Array<VkImage> images;
    if (!check_device_result(EnumerateImages(new_sc, images), EVulkanFaultOperation::SwapchainGetImages)) {
        return false;
    }
    if (!check_device_result(
            CreateImageResources(images, new_format, create_info.imageExtent, new_swapchain_images),
            EVulkanFaultOperation::SwapchainSemaphoreCreate
        )) {
        return false;
    }
    // Ordinary identity replacement remains transactional. Forced recovery
    // has already retired its unusable native bundle, so this is a no-op for
    // that path.
    retire_old_resources();

    if (candidate_owns_surface) {
        const VkSurfaceKHR old_surface = surface;
        surface                        = std::exchange(candidate_surface, VK_NULL_HANDLE);
        if (old_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(device.GetInstance(), old_surface, VK_NULL_HANDLE);
        }
    }
    surface_info = _info.surface;
    handle       = new_sc;
    new_sc       = VK_NULL_HANDLE;
    size         = {create_info.imageExtent.width, create_info.imageExtent.height};
    format       = new_format;
    swapchain_images.swap(new_swapchain_images);
    present_serial                = 0;
    const bool presentation_ready = IsPresentationReady();
    if (presentation_ready) {
        native_surface_recreate_pending_ = false;
    }
    return presentation_ready;
}
VkResult VkSwapchain::EnumerateImages(VkSwapchainKHR swapchain, Array<VkImage>& images) const {
    for (uint32 attempt = 0; attempt < 4; ++attempt) {
        uint32   count  = 0;
        VkResult result = vkGetSwapchainImagesKHR(device.GetDevice(), swapchain, &count, nullptr);
        if (result != VK_SUCCESS || device.IsFaulted()) {
            return result;
        }
        if (count == 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        images.assign(count, VK_NULL_HANDLE);
        result = vkGetSwapchainImagesKHR(device.GetDevice(), swapchain, &count, images.data());
        if (result == VK_INCOMPLETE) {
            continue;
        }
        if (result != VK_SUCCESS || device.IsFaulted()) {
            return result;
        }
        if (count == 0) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        images.resize(count);
        return VK_SUCCESS;
    }
    LOG_ERROR("Swapchain image enumeration remained incomplete after four attempts.");
    return VK_ERROR_INITIALIZATION_FAILED;
}

VkResult VkSwapchain::CreateImageResources(
    std::span<const VkImage> images,
    EPixelFormat             image_format,
    VkExtent2D               extent,
    Array<SwapchainImage>&   resources
) {
    resources.resize(images.size());
    const VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (size_t i = 0; i < images.size(); ++i) {
        const auto result = vkCreateSemaphore(
            device.GetDevice(), &semaphore_info, VK_NULL_HANDLE, &resources[i].render_finished_semaphore
        );
        if (result != VK_SUCCESS) {
            resources[i].render_finished_semaphore = VK_NULL_HANDLE;
            return result;
        }
        if (device.IsFaulted()) {
            return result;
        }
        device.SetResourceName(
            uint64(resources[i].render_finished_semaphore), VK_OBJECT_TYPE_SEMAPHORE, "RenderFinishedSemaphore_" + std::to_string(i)
        );
    }
    for (size_t i = 0; i < images.size(); ++i) {
        resources[i].texture = MoerNew(VulkanTexture)(
            TextureInfo{
                ETextureDimension::TEX_2D, ETextureUsageFlags::PRESENT, image_format, EClearAttachment{}, {extent.width, extent.height, 1}
            },
            &device,
            images[i]
        );
        resources[i].view = TextureView(resources[i].texture);
    }
    return VK_SUCCESS;
}

VkSwapchain::~VkSwapchain() {
    DestroyImages(swapchain_images);
    if (handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device.GetDevice(), handle, VK_NULL_HANDLE);
        handle = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device.GetInstance(), surface, VK_NULL_HANDLE);
        surface = VK_NULL_HANDLE;
    }
}
void VkSwapchain::DestroyImages(Array<SwapchainImage>& _images) {
    for (const SwapchainImage& image : _images) {
        if (image.texture != nullptr) {
            MoerDelete(image.texture);
        }
        if (image.render_finished_semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device.GetDevice(), image.render_finished_semaphore, VK_NULL_HANDLE);
        }
    }
    _images.clear();
}

VkSemaphore VkSwapchain::GetRenderFinishedSemaphore(uint32 _image_index) const {
    return swapchain_images[_image_index % swapchain_images.size()].render_finished_semaphore;
}
TextureView VkSwapchain::GetSwapchainImage(uint32 _index) const {
    return swapchain_images[_index % swapchain_images.size()].view;
}
VkSwapchain::AcquireResult VkSwapchain::AcquireNextImage(VkSemaphore _ready_semaphore, uint64 _timeout) {
    assert(_ready_semaphore != VK_NULL_HANDLE);
    uint32                      image_index = UINT32_MAX;
    const VulkanOperationResult outcome     = device.AcquireNextImage(
        handle,
        _timeout,
        _ready_semaphore,
        VK_NULL_HANDLE,
        &image_index,
        VulkanOperationContext{
                .operation  = EVulkanFaultOperation::AcquireNextImage,
                .queue_type = EQueueType::Graphics,
                .queue      = device.GetGraphicsQueue(),
                .timeline   = present_serial,
        }
    );
    if ((outcome.result == VK_SUCCESS || outcome.result == VK_SUBOPTIMAL_KHR) && image_index != UINT32_MAX) {
        return {outcome, _ready_semaphore, image_index};
    }
    return {outcome, VK_NULL_HANDLE, UINT32_MAX};
}

VkSwapchain::PresentResult
VkSwapchain::Present(VkQueue _queue, uint32 _index, uint64 _timeline, uint64 _work_serial) {
    PresentResult present_result{};
    if (_index == UINT32_MAX) {
        present_result.outcome = {
            EVulkanOperationStatus::Retry, VK_NOT_READY
        };
        return present_result;
    }
    VkSemaphore      finished_semaphores[] = {GetRenderFinishedSemaphore(_index)};
    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = finished_semaphores;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &handle;
    present_info.pImageIndices      = &_index;
    present_result.outcome          = device.PresentOnQueue(
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
    present_result.accepted =
        present_result.outcome.result == VK_SUCCESS ||
        present_result.outcome.result == VK_SUBOPTIMAL_KHR;
    ++present_serial;
    return present_result;
}

} // namespace Moer::Render
