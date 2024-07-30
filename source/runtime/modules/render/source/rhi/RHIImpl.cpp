#include "RHIImpl.h"
#include "rhi/RHIResource.h"
namespace Moer::Render{
    // RHIViewportRef RenderDevice::CreateViewport(const RHIViewportInitializer& _init) {
    //     return impl->CreateViewport(_init);
    // }

    // BackBufferInfo RenderDevice::GetNextBackBufferInfo(RHIViewport* _viewport) {
    //     return impl->GetNextBackBufferInfo(_viewport);
    // }

    FenceRef RenderDevice::CreateTimeline() {
        return impl->CreateTimeline();
    }

    FenceRef RenderDevice::CreatePresentFence() {
        return impl->CreatePresentFence();
    }

    PipelineHandle RenderDevice::CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) {
        return impl->CreatePipeline(std::move(_pso_info), std::move(_shaders));
    }

    PipelineHandle RenderDevice::CreatePipeline(PipelineShaderInfo&& _shaders) {
        return impl->CreatePipeline(std::move(_shaders));
    }

    TextureRef RenderDevice::CreateTexture(Extent2D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint32_t _array_size) {
        return impl->CreateTexture(_size, _format, _usage, _mip_cnt, _array_size);
    }

    TextureRef RenderDevice::CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint32_t _array_size) {
        return impl->CreateTexture(_size, _format, _usage, _mip_cnt, _array_size);
    }

    DepthBufferRef RenderDevice::CreateDepthBuffer(Extent2D _size, EPixelFormat _format, uint32_t _array_size) {
        return impl->CreateDepthBuffer(_size, _format, _array_size);
    }

    SwapchainRef RenderDevice::CreateSwapchain(const SwapchainCreateInfo& _info) {
        return impl->CreateSwapchain(_info);
    }
}