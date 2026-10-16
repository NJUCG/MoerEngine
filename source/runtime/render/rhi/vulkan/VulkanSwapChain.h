//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "VulkanCommon.h"
#include "VulkanFault.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

class VulkanDevice;
class VulkanTexture;

class VkSwapchain : public Swapchain {
public:
    struct AcquireResult {
        VulkanOperationResult outcome{};
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
    VkSwapchain(VulkanDevice& _device, const SwapchainCreateInfo& _info);
    ~VkSwapchain() override;
    [[nodiscard]] bool          Recreate(const SwapchainCreateInfo& _info) override;
    [[nodiscard]] AcquireResult AcquireNextImage(VkSemaphore _ready_semaphore, uint64 _timeout = UINT64_MAX);
    [[nodiscard]] TextureView   GetSwapchainImage(uint _index) const;
    [[nodiscard]] VkSemaphore   GetRenderFinishedSemaphore(uint _image_index) const;
    [[nodiscard]] PresentResult Present(
        VkQueue                             _queue,
        uint                                _image_index,
        uint64                              _timeline,
        uint64                              _work_serial,
        const PresentationCompletionTicket& _completion
    );
    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return handle != VK_NULL_HANDLE && surface != VK_NULL_HANDLE && surface_info.IsValid() &&
               size.x != 0 && size.y != 0;
    }
    [[nodiscard]] WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return surface_info.GetIdentity();
    }

    [[nodiscard]] bool WaitFrameInFlight();
    [[nodiscard]] PresentationCompletionTicket
    ReservePresentCompletion(PresentationCompletionIdentity _identity);

private:
    struct SwapchainImage {
        VulkanTexture* texture{nullptr};
        TextureView    view{};
        VkSemaphore    render_finished_semaphore{VK_NULL_HANDLE};
    };

    struct PresentSlot {
        VkFence                      fence{VK_NULL_HANDLE};
        PresentationCompletionTicket completion{};
        uint64                       generation{0};
    };

    [[nodiscard]] bool CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
    [[nodiscard]] bool PreparePresentFence(uint64 _present_serial);
    void               DestroyImages(Array<SwapchainImage>& _images);
    void               DestroyPresentSlots(Array<PresentSlot>& _slots);

    Array<SwapchainImage> swapchain_images;
    Array<PresentSlot>    present_slots;

    VkSwapchainKHR       handle  = VK_NULL_HANDLE;
    VkSurfaceKHR         surface = VK_NULL_HANDLE;
    SwapchainSurfaceInfo surface_info{};
    VulkanDevice&        device;
    uint64               present_serial = 0;

    static constexpr uint max_frames_in_flight = 3;

    bool native_surface_recreate_pending_ = false;
};
} // namespace Moer::Render

#endif // VULKAN_SWAP_CHAIN_H
