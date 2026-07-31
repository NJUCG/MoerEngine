//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "PixelFormat.h"
#include "misc/CountableRef.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include "VulkanFault.h"
#include "VulkanPlatform.h"
#include <atomic>
namespace Moer::Render {
VkSurfaceFormatKHR      ChooseSwapSurfaceFormat(
         const Array<VkSurfaceFormatKHR>& _available_formats,
         EPixelFormat                     _preferred_format,
         bool                             _prefer_hdr = false
     );
VkPresentModeKHR
ChooseSwapPresentMode(const Array<VkPresentModeKHR>& _available_present_modes, bool vsync = false);
VkExtent2D
ChooseSwapExtent(uint32_t* _width, uint32_t* _height, const VkSurfaceCapabilitiesKHR& _capabilities);

class VkSwapchain : public Swapchain {
public:
    struct AcquireResult {
        VulkanOperationResult outcome;
        VkSemaphore           ready_semaphore{VK_NULL_HANDLE};
        uint32                image_index{UINT32_MAX};
        uint64                present_timeline{0};

        [[nodiscard]] bool HasImage() const {
            return image_index != UINT32_MAX;
        }
    };
    struct PresentResult {
        VulkanOperationResult outcome{};
        VkFence               completion_fence{VK_NULL_HANDLE};
        bool                  accepted{false};
        bool                  wsi_enqueued{false};
        bool                  uses_present_fence{false};
    };
    friend VkCommandQueue;
    VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info);
    ~VkSwapchain();
    [[nodiscard]] bool Recreate(const SwapchainCreateInfo& _info) override;
    [[nodiscard]] bool
    CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
    AcquireResult AquireNextImage(
        VkSemaphore _ready_semaphore,
        uint64      _timeout = UINT64_MAX
    );
    TextureView                         GetSwapchainImage(uint _index);
    VkSemaphore                         GetRenderFinishedFence(uint _image_index);
    PresentResult Present(
        VkQueue _queue,
        uint    _image_index,
        uint64  _timeline,
        uint64  _work_serial,
        const PresentationCompletionTicket& _completion
    );
    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return handle != VK_NULL_HANDLE &&
               surface != VK_NULL_HANDLE &&
               surface_info.IsValid() &&
               surface_info.GetIdentity().IsValid() &&
               size.x != 0 &&
               size.y != 0;
    }
    [[nodiscard]] WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return surface_info.GetIdentity();
    }

    bool               WaitFrameInFlight();
    PresentationCompletionTicket ReservePresentCompletion(
        PresentationCompletionIdentity _identity
    );
    VkFence            GetInFlightFence(uint64 _image_idx);
    VkSurfaceFormatKHR GetSurfaceFormat() const {
        return fmt;
    }
    VkSurfaceFormatKHR fmt;

    Array<VkSemaphore>          render_finished_fences;
    Array<class VulkanTexture*> swapchain_textures;
    Array<TextureView>          swapchain_views;
    Array<VkFence>              in_flight_fences;

    VkSwapchainKHR      handle  = VK_NULL_HANDLE;
    VkSurfaceKHR        surface = VK_NULL_HANDLE;
    SwapchainSurfaceInfo surface_info{};
    class VulkanDevice& device;
    uint64              image_idx            = 0; // present queue timeline value
    uint                max_frames_in_flight = 3;

    std::atomic_uint64_t present_timeline = 0;

private:
    bool PreparePresentFence(uint64 _present_idx);

    struct PresentFenceSlot {
        PresentationCompletionTicket completion{};
        uint64                       generation{0};
    };

private:
    Array<PresentFenceSlot> present_fence_slots{};
    bool                    native_surface_recreate_pending_ = false;
};
} // namespace Moer::Render

#endif // VULKAN_SWAP_CHAIN_H
