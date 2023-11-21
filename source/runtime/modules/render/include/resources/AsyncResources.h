#ifndef MOER_ENGINE_ASYNC_RESOURCES_H
#define MOER_ENGINE_ASYNC_RESOURCES_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <vector>

//In Editor mode, UI runs and renders on main thread, but in application mode,
//everything runs on render thread, so we need to create a virtual swap chain
class VirtualViewport {
public:
    RHIShaderResourceViewRef GetSRVMainThread();

    //call on render thread
    RHIUnorderedAccessViewRef GetUAVRenderThread();

    //call on main thread
    void                    OnResize(Extent2D extent);
    static VirtualViewport* Create(const RHITextureCreateInfo& create_info);

private:
    VirtualViewport(const RHITextureCreateInfo& create_info);
    ~VirtualViewport();

    void InitRenderThread();
    void ResizeRenderThread(Extent2D extent);
    //resource copy on main thread
    RHITextureRef upload_texture;

    RHITextureCreateInfo upload_texture_create_info;

    //resource on render thread
    std::vector<RHITextureRef>             textures_render_thread;
    std::vector<RHIUnorderedAccessViewRef> texture_uavs_render_thread;

// in Application mode, Present operations happens on render thread
#if !defined(EDITOR_MODE_ON)
    RHIViewportRef viewport;
#endif
};

#endif//MOER_ENGINE_ASYNC_RESOURCES_H