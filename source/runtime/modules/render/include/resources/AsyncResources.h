#ifndef MOER_ENGINE_ASYNC_RESOURCES_H
#define MOER_ENGINE_ASYNC_RESOURCES_H

#include "PixelFormat.h"
#include "math/Base.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

namespace Moer {
    struct VirtualViewportInfo {
        std::string    name;
        Moer::Vector2i extent;
        EPixelFormat   format;
        uint32_t       back_buffer_count = 2;
    };

    struct VirtualViewportCreateInfo {
        std::string    name;
        Moer::Vector2i extent;
        EPixelFormat   format;
        uint32_t       back_buffer_count = 2;
    };

    struct VirtualViewportNextBackBufferInfo {
        uint32_t  backbuffer_index;
        RHIFence* backbuffer_ready_fence;
    };
    //In Editor mode, UI runs and renders on main thread, but in application mode,
    //everything runs on render thread, so we need to create a virtual swap chain
    class VirtualViewport {
    public:
        VirtualViewport(const VirtualViewportCreateInfo& create_info);
        ~VirtualViewport();
        //call on main thread
        void OnResize(Moer::Vector2i extent);

        //call from render thread
        VirtualViewportNextBackBufferInfo GetNextBackBuffer();

        RHIUAVRef GetNextBackBufferUAV(uint32_t index);

        RHIUAVRef     GetDepthBufferUAV();
        RHITextureRef getDepthTexture();

        void Present(RHIFenceRef _render_fence);

        const VirtualViewportInfo& GetInfo() const;

        RHISRV* GetPresentTextureSRV();

    private:
        void InitRenderThread();
        void ResizeRenderThread(Moer::Vector2i extent);

// in Application mode, Present operations happens on render thread
#if !defined(EDITOR_MODE_ON)
        RHIViewportRef viewport;
#endif
        class Impl;
        Impl* impl;
    };

}// namespace Moer

#endif//MOER_ENGINE_ASYNC_RESOURCES_H