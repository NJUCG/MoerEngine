#include "resources/AsyncResources.h"
#include "Core.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"

UploadTexture::UploadTexture(const RHITextureCreateInfo& create_info) {
    // Implementation of UploadTexture constructor
    // ...
    upload_texture_create_info = create_info;
    upload_texture             = g_rhi->RHICreateTexture(upload_texture_create_info);

    FunctionGraphTask::ConstructAndDispatchWhenReady(
        [this]() {
            InitRenderThread();
        },
        nullptr,
        EThread::ERenderThread);
}

UploadTexture* UploadTexture::Create(const RHITextureCreateInfo& create_info) {
    // Implementation of Create method
    // ...
    return new UploadTexture(create_info);
    return nullptr;
}

UploadTexture::~UploadTexture() {
    // Implementation of UploadTexture destructor
    // ...
}

void UploadTexture::InitRenderThread() {
    // Implementation of InitRenderThread method
    // ...
    assert(Moer::IsCurrentlyRenderThread());

    textures_render_thread.resize(3);
    texture_uavs_render_thread.resize(3);
    for (int i = 0; i < 3; ++i) {
        textures_render_thread[i] = g_rhi->RHICreateTexture(upload_texture_create_info);
        texture_uavs_render_thread[i] =
            g_rhi->RHICreateUnorderedAccessView(textures_render_thread[i],
                                                RHIViewInfo::CreateTextureUAVInfo()
                                                    .SetArrayRange(0, 1)
                                                    .SetMipLevel(0)
                                                    .SetDimension(ETextureDimension::TEX_2D));
    }
}

void UploadTexture::OnResize(Extent2D extent) {
    if (extent == upload_texture_create_info.extent) { return; }
    // Implementation of OnResize method
    // ...
    FunctionGraphTask::ConstructAndDispatchWhenReady(
        [this, extent]() {
            ResizeRenderThread(extent);
        },
        nullptr,
        EThread::ERenderThread);
}

void UploadTexture::ResizeRenderThread(Extent2D extent) {
    // Implementation of ResizeRenderThread method
    // ...
    assert(Moer::IsCurrentlyRenderThread());
}

RHIShaderResourceViewRef UploadTexture::GetSRVMainThread() {
    // Implementation of GetSRVMainThread method
    // ...
    return nullptr;
}

RHIUnorderedAccessViewRef UploadTexture::GetUAVRenderThread() {
    // Implementation of GetUAVMainRenderThread method
    // ...
    return nullptr;
}