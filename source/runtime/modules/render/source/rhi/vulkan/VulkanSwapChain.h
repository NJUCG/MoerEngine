//
// Created by 74535 on 2023/10/5.
//

#ifndef VULKAN_SWAP_CHAIN_H
#define VULKAN_SWAP_CHAIN_H

#include "PixelFormat.h"
#include "misc/CountableRef.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "window/WindowContext.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <volk.h>
namespace Moer::Render {
    struct SwapChainBuffer {
        // VkImage image;
        class VulkanRHITexture* image;
        // VkImageView view;
        class VulkanImageView* view;
    };
    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR  capabilities;
        Array<VkSurfaceFormatKHR> formats;
        Array<VkPresentModeKHR>   present_modes;
    };
    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface);
    VkSurfaceFormatKHR      ChooseSwapSurfaceFormat(const Array<VkSurfaceFormatKHR>& _available_formats, EPixelFormat _preferred_format, bool _prefer_hdr = false);
    VkPresentModeKHR        ChooseSwapPresentMode(const Array<VkPresentModeKHR>& _available_present_modes, bool vsync = false);
    VkExtent2D              ChooseSwapExtent(uint32_t* _width, uint32_t* _height, const VkSurfaceCapabilitiesKHR& _capabilities);
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
        friend VkCommandQueue;
        VkSwapchain(RenderDevice::Impl& _device, const SwapchainCreateInfo& _info);
        ~VkSwapchain();
        void                                Recreate(const SwapchainCreateInfo& _info) override;
        void                                CreateOrRecreate(const SwapchainCreateInfo& _info, bool _force_recreate = false);
        std::tuple<VkSemaphore, uint, uint> AquireNextImage();
        TextureView                         GetSwapchainImage(uint _index);
        VkSemaphore                         GetImageReadyFence(uint _index);
        VkSemaphore                         GetRenderFinishedFence();
        void                                Present(VkQueue _queue, uint _image_index);
        void                                Sync() override;

        void               WaitFrameInFlight();
        void               WaitFrameInFlight(uint64 _image_idx);
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
        uint           max_frames_in_flight = 3;

        std::atomic_uint64_t present_timeline = 0;

    private:
        void OnFinishPresent(uint64 _image_idx);
        void EnqueuePresent(uint64 _present_idx);

    private:
        Array<std::jthread> present_threads;
        std::atomic<uint>   cur_present_cnt = 0;
    };
}// namespace Moer::Render

#endif// VULKAN_SWAP_CHAIN_H
