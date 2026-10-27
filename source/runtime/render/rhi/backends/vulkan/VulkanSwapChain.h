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
        VulkanOperationResult         outcome{};
        VkSemaphore                   ready_semaphore{VK_NULL_HANDLE};
        uint32                        image_index{UINT32_MAX};

        [[nodiscard]] bool HasImage() const {
            return image_index != UINT32_MAX;
        }
    };
    struct PresentResult {
        VulkanOperationResult outcome{};
        bool                  accepted{false};
    };
    VkSwapchain(VulkanDevice& _device, const SwapchainCreateInfo& _info);
    ~VkSwapchain() override;
    [[nodiscard]] bool          Recreate(const SwapchainCreateInfo& _info) override;
    [[nodiscard]] AcquireResult AcquireNextImage(VkSemaphore _ready_semaphore, uint64 _timeout = UINT64_MAX);
    [[nodiscard]] TextureView   GetSwapchainImage(uint _index) const;
    [[nodiscard]] VkSemaphore   GetRenderFinishedSemaphore(uint _image_index) const;
    [[nodiscard]] PresentResult
                       Present(VkQueue _queue, uint _image_index, uint64 _timeline, uint64 _work_serial);
    [[nodiscard]] bool IsPresentationReady() const noexcept override {
        return handle != VK_NULL_HANDLE && surface != VK_NULL_HANDLE && surface_info.IsValid() &&
               size.x != 0 && size.y != 0;
    }
    [[nodiscard]] WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return surface_info.GetIdentity();
    }

private:
    struct SwapchainImage {
        VulkanTexture*               texture{nullptr};
        TextureView                  view{};
        VkSemaphore                  render_finished_semaphore{VK_NULL_HANDLE};
    };

    [[nodiscard]] bool CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
    void               DestroyImages(Array<SwapchainImage>& _images);

    Array<SwapchainImage> swapchain_images;

    VkSwapchainKHR       handle  = VK_NULL_HANDLE;
    VkSurfaceKHR         surface = VK_NULL_HANDLE;
    SwapchainSurfaceInfo surface_info{};
    VulkanDevice&        device;
    uint64               present_serial = 0;

    bool native_surface_recreate_pending_ = false;
};
inline VkSwapchain* ResourceCast(Swapchain* resource) {
    return static_cast<VkSwapchain*>(resource);
}
} // namespace Moer::Render

#endif // VULKAN_SWAP_CHAIN_H
