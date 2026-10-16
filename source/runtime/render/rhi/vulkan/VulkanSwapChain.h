//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "PixelFormat.h"
#include "VulkanFault.h"
#include "VulkanPlatform.h"
#include "misc/CountableRef.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
namespace Moer::Render {

class VkSwapchain : public Swapchain {
public:
    struct AcquireResult {
        VulkanOperationResult outcome;
        VkSemaphore           ready_semaphore{VK_NULL_HANDLE};
        uint32                image_index{UINT32_MAX};

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
    VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info);
    ~VkSwapchain() override;
    [[nodiscard]] bool Recreate(const SwapchainCreateInfo& _info) override;
    AcquireResult      AquireNextImage(VkSemaphore _ready_semaphore, uint64 _timeout = UINT64_MAX);
    TextureView        GetSwapchainImage(uint _index);
    VkSemaphore        GetRenderFinishedFence(uint _image_index);
    PresentResult      Present(
             VkQueue                             _queue,
             uint                                _image_index,
             uint64                              _timeline,
             uint64                              _work_serial,
             const PresentationCompletionTicket& _completion
         );
    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return handle != VK_NULL_HANDLE && surface != VK_NULL_HANDLE && surface_info.IsValid() &&
               surface_info.GetIdentity().IsValid() && size.x != 0 && size.y != 0;
    }
    [[nodiscard]] WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return surface_info.GetIdentity();
    }

    bool                         WaitFrameInFlight();
    PresentationCompletionTicket ReservePresentCompletion(PresentationCompletionIdentity _identity);

private:
    Array<VkSemaphore>          render_finished_fences;
    Array<class VulkanTexture*> swapchain_textures;
    Array<TextureView>          swapchain_views;
    Array<VkFence>              in_flight_fences;

    VkSwapchainKHR       handle  = VK_NULL_HANDLE;
    VkSurfaceKHR         surface = VK_NULL_HANDLE;
    SwapchainSurfaceInfo surface_info{};
    class VulkanDevice&  device;
    uint64               image_idx            = 0; // present queue timeline value
    uint                 max_frames_in_flight = 3;

    [[nodiscard]] bool CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
    bool               PreparePresentFence(uint64 _present_idx);

    struct PresentFenceSlot {
        PresentationCompletionTicket completion{};
        uint64                       generation{0};
    };

    Array<PresentFenceSlot> present_fence_slots{};
    bool                    native_surface_recreate_pending_ = false;
};
} // namespace Moer::Render

#endif // VULKAN_SWAP_CHAIN_H
