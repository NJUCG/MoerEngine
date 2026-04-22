//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "PixelFormat.h"
#include "misc/CountableRef.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include "window/WindowContext.h"

#include "VulkanPlatform.h"
namespace Moer::Render {
struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR  capabilities;
    Array<VkSurfaceFormatKHR> formats;
    Array<VkPresentModeKHR>   present_modes;
};
SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface);
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
    friend VkCommandQueue;
    VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info);
    ~VkSwapchain();
    void Recreate(const SwapchainCreateInfo& _info) override;
    void CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
    std::tuple<VkSemaphore, uint, uint> AquireNextImage();
    TextureView                         GetSwapchainImage(uint _index);
    VkSemaphore                         GetImageReadyFence(uint _index);
    VkSemaphore                         GetRenderFinishedFence();
    void                                Present(VkQueue _queue, uint _image_index);
    void                                Sync() override;

    void               WaitFrameInFlight();
    void               WaitFrameInFlight(uint64 _image_idx);
    VkFence            GetInFlightFence(uint64 _image_idx);
    VkSurfaceFormatKHR GetSurfaceFormat() const {
        return fmt;
    }
    VkSurfaceFormatKHR fmt;

    Array<VkSemaphore>          image_ready_fences;
    Array<VkSemaphore>          render_finished_fences;
    Array<class VulkanTexture*> swapchain_textures;
    Array<TextureView>          swapchain_views;
    Array<VkFence>              in_flight_fences;

    VkSwapchainKHR      handle  = VK_NULL_HANDLE;
    VkSurfaceKHR        surface = VK_NULL_HANDLE;
    class VulkanDevice& device;
    uint64              image_idx            = 0; // present queue timeline value
    uint                max_frames_in_flight = 3;
};
} // namespace Moer::Render

#endif // VULKAN_SWAP_CHAIN_H
