#ifndef MOER_ENGINE_ASYNC_RESOURCES_H
#define MOER_ENGINE_ASYNC_RESOURCES_H

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include <vector>
class UploadTexture {
public:
    RHIShaderResourceViewRef GetSRVMainThread();

    //call on render thread
    RHIUnorderedAccessViewRef GetUAVRenderThread();

    //call on main thread
    void                  OnResize(Extent2D extent);
    static UploadTexture* Create(const RHITextureCreateInfo& create_info);

private:
    UploadTexture(const RHITextureCreateInfo& create_info);
    ~UploadTexture();

    void InitRenderThread();
    void ResizeRenderThread(Extent2D extent);
    //resource copy on main thread
    RHITextureRef upload_texture;

    RHITextureCreateInfo upload_texture_create_info;

    //resource on render thread
    std::vector<RHITextureRef>             textures_render_thread;
    std::vector<RHIUnorderedAccessViewRef> texture_uavs_render_thread;
};

#endif//MOER_ENGINE_ASYNC_RESOURCES_H