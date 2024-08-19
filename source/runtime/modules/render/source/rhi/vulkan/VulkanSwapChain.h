//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "misc/CountableRef.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "window/WindowContext.h"

#include <vulkan/vulkan_core.h>
namespace Moer::Render {
    struct SwapChainBuffer {
        // VkImage image;
        class VulkanRHITexture* image;
        // VkImageView view;
        class VulkanImageView* view;
    };
    VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const Moer::Array<VkSurfaceFormatKHR>& _available_formats, bool _prefer_hdr = false);
    VkPresentModeKHR   ChooseSwapPresentMode(const Moer::Array<VkPresentModeKHR>& _available_present_modes, bool vsync = false);
    VkExtent2D         ChooseSwapExtent(uint32_t* _width, uint32_t* _height, const VkSurfaceCapabilitiesKHR& _capabilities);
    class VulkanSwapChain {
    public:
        VulkanSwapChain() {}
        VulkanSwapChain(VkInstance _instance, Moer::WindowHandle* _window, VulkanDevice* _device);
        ~VulkanSwapChain();
        void     Connect(VkInstance _instance, VkSurfaceKHR _surface, VulkanDevice* _device);
        void     Init(uint32_t* width, uint32_t* height, bool vsync);
        uint32_t AcquireNextImage(VkSemaphore _aquire_semaphore);
        VkResult Present(VkQueue _queue, VkSemaphore _render_finished);
        void     Cleanup();

        VkSurfaceFormatKHR GetSurfaceFormat() const { return surface_format; }

    private:
        void Create(uint32_t* width, uint32_t* height, bool vsync);
        void Recreate();

    private:
        friend class VulkanRHIViewport;
        friend class VulkanViewport;
        friend class ::VulkanRHIImpl;
        VkInstance     m_instance;
        VulkanDevice*  m_device;
        VkSwapchainKHR m_swap_chain;
        VkSurfaceKHR   m_surface;

        Moer::Array<VkImage> m_swap_chain_images;

        uint32_t current_image_index;

        VkExtent2D         extent;
        VkFormat           image_format;
        VkSurfaceFormatKHR surface_format;

    private:
        VkImageView CreateImageView(VkImage _image, VkFormat _format, uint32_t mipLevels, VkImageAspectFlags aspectMask);
    };

    class VkSwapchain : public Swapchain {
    public:
        VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info);
        ~VkSwapchain();
        void                                Recreate(const SwapchainCreateInfo& _info);
        void                                CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
        std::tuple<VkSemaphore, uint, uint> AquireNextImage();
        TextureView                         GetSwapchainImage(uint _index);
        VkSemaphore                         GetImageReadyFence(uint _index);
        VkSemaphore                         GetRenderFinishedFence();
        void                                Present(VkQueue _queue, uint _image_index);

        void               WaitFrameInFlight();
        VkFence            GetInFlightFence(uint64 _image_idx);
        VkSurfaceFormatKHR GetSurfaceFormat() const { return fmt; }
        VkSurfaceFormatKHR fmt;

        Array<VkSemaphore>          image_ready_fences;
        Array<VkSemaphore>          render_finished_fences;
        Array<class VulkanTexture*> swapchain_textures;
        Array<TextureView>          swapchain_views;
        Array<VkFence>              in_flight_fences;

        VkSwapchainKHR handle  = VK_NULL_HANDLE;
        VkSurfaceKHR   surface = VK_NULL_HANDLE;
        VulkanDevice&  device;
        uint64         image_idx            = 0;
        uint           max_frames_in_flight = 2;
    };
}// namespace Moer::Render

#endif// VULKAN_SWAP_CHAIN_H
