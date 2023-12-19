#include "resources/AsyncResources.h"
#include "Core.h"
#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"

#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"

#include <atomic>
#include <stdint.h>
#include <vector>
namespace Moer {
    // struct VirtualViewport::VirtualViewportData {
    //     VirtualViewportData()  = default;
    //     ~VirtualViewportData() = default;

    //     RHITextureCreateInfo upload_texture_create_info;

    //     RHIFenceRef present_fence;

    //     RHITextureRef present_texture;

    //     std::vector<RHITextureRef>             swapchain_textures;
    //     std::vector<RHIUnorderedAccessViewRef> swapchain_uavs;
    //     std::atomic_uint64_t                   frame_index = 0;

    //     RHICommandQueue* copy_queue;

    //     RHIGraphicsCommandList* copy_cmd_list;
    // };

    class VirtualViewport::Impl {

    public:
        Impl(const VirtualViewportCreateInfo& create_info);
        ~Impl();
        //call on main thread
        void OnResize(Moer::Vector2i extent);

        //call from render thread
        VirtualViewportNextBackBufferInfo GetNextBackBuffer();

        RHIUnorderedAccessViewRef GetNextBackBufferUAV(uint32_t index);

        void Present(RHIFenceRef _render_fence);

        const VirtualViewportInfo& GetInfo() const { return info; }

    private:
        friend VirtualViewport;
        void InitRenderThread();
        void ResizeRenderThread(Moer::Vector2i extent);

// in Application mode, Present operations happens on render thread
#if !defined(EDITOR_MODE_ON)
        RHIViewportRef viewport;
#endif
        VirtualViewportInfo info;

    private:
        RHITextureCreateInfo upload_texture_create_info;

        RHIFenceRef present_fence;

        RHITextureRef present_texture;

        Moer::Array<RHITextureRef>             swapchain_textures;
        Moer::Array<RHIUnorderedAccessViewRef> swapchain_uavs;
        uint64_t                               frame_index     = 0;
        uint64_t                               presented_index = 0;

        RHICommandQueue* copy_queue;

        RHICopyCommandList* copy_cmd_list;
    };
    VirtualViewport::VirtualViewport(const VirtualViewportCreateInfo& create_info) {
        // Implementation of UploadTexture constructor
        // ...
        impl = MoerNew(Impl)(create_info);
    }

    VirtualViewport::~VirtualViewport() {
        // Implementation of UploadTexture destructor
        // ...
        MoerDelete(impl);
    }

    void VirtualViewport::InitRenderThread() {
        impl->InitRenderThread();
    }

    void VirtualViewport::OnResize(Moer::Vector2i extent) {
        // Implementation of OnResize method
        // ...
        impl->OnResize(extent);
    }

    void VirtualViewport::Present(RHIFenceRef _render_fence) {
        impl->Present(_render_fence);
    }

    void VirtualViewport::ResizeRenderThread(Moer::Vector2i extent) {
        // Implementation of ResizeRenderThread method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
        impl->ResizeRenderThread(extent);
    }

    const VirtualViewportInfo& VirtualViewport::GetInfo() const {
        return impl->GetInfo();
    }

    VirtualViewportNextBackBufferInfo VirtualViewport::GetNextBackBuffer() {
        return impl->GetNextBackBuffer();
    }

    RHIUnorderedAccessViewRef VirtualViewport::GetNextBackBufferUAV(uint32_t index) {
        return impl->GetNextBackBufferUAV(index);
    }

    VirtualViewport::Impl::~Impl() {
    }
    VirtualViewport::Impl::Impl(const VirtualViewportCreateInfo& create_info) {

        present_fence = g_rhi->RHICreateFence({.usage = EFenceUsage::TIMELINE});

        copy_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
        copy_cmd_list = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());

        info.back_buffer_count = create_info.back_buffer_count;
        info.extent            = create_info.extent;
        info.format            = create_info.format;
        info.name              = create_info.name;

        upload_texture_create_info = RHITextureCreateInfo::Create2D("virtual viewport",
                                                                    create_info.extent,
                                                                    create_info.format)
                                         .SetArraySize(1)
                                         .SetNumMips(1)
                                         .SetClearAttachment({})
                                         .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED)
                                         .SetUsageFlags(
                                             ETextureUsageFlags::COLOR_ATTACHMENT |
                                             ETextureUsageFlags::TRANSFER_SRC |
                                             ETextureUsageFlags::SRGB |
                                             ETextureUsageFlags::SAMPLED);

        auto present_texture_create_info = upload_texture_create_info;

        present_texture = g_rhi->RHICreateTexture(present_texture_create_info.SetUsageFlags(
            ETextureUsageFlags::COLOR_ATTACHMENT |
            ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::TRANSFER_DST));

        auto* texture = g_rhi->RHIGetViewportBackBufferUAV(g_rhi->RHIGetMainViewport(), 0)->GetTexture();
        swapchain_textures.resize(info.back_buffer_count);
        swapchain_uavs.resize(info.back_buffer_count);
        for (int i = 0; i < info.back_buffer_count; ++i) {
            swapchain_textures[i] = g_rhi->RHICreateTexture(upload_texture_create_info);
            swapchain_uavs[i] =
                g_rhi->RHICreateUnorderedAccessView(swapchain_textures[i],
                                                    RHIViewInfo::CreateTextureUAVInfo()
                                                        .SetArrayRange(0, 1)
                                                        .SetMipLevel(0)
                                                        .SetFormat(info.format)
                                                        .SetDimension(ETextureDimension::TEX_2D));
        }
        RHIFenceRef fence = g_rhi->RHICreateFence({.usage = EFenceUsage::AQUIRE_NEXT_FRAME});

        RHIBarrierDependencyInfo           barrier_info;
        Moer::Array<RHITextureBarrierInfo> barriers(info.back_buffer_count + 1);
        for (uint32_t i = 0; i < info.back_buffer_count; ++i) {
            barriers[i].SetDstTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT);
            barriers[i].SetTexture(swapchain_textures[i]);
            barriers[i].SetSubResourceRange({});
            barriers[i].SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED);
        }
        barriers[info.back_buffer_count].SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_DST);
        barriers[info.back_buffer_count].SetTexture(present_texture);
        barriers[info.back_buffer_count].SetSubResourceRange({});

        barrier_info.texture_barrier_count = barriers.size();
        barrier_info.p_texture_barriers    = barriers.data();
        copy_cmd_list->Open();
        copy_cmd_list->SetPipelineBarrier(barrier_info);
        copy_cmd_list->Close();
        RHISubmitInfo submit_info;
        submit_info.Signal(fence, 1);
        copy_queue->SubmitCommands(1, copy_cmd_list, &submit_info);
        fence->Wait(1);
    }

    void VirtualViewport::Impl::InitRenderThread() {
        // Implementation of InitRenderThread method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
    }

    void VirtualViewport::Impl::OnResize(Moer::Vector2i extent) {
    }

    void VirtualViewport::Impl::Present(RHIFenceRef _render_fence) {
        // Implementation of Present method
        // ...
        assert(Moer::IsCurrentlyGameThread() && "Present should be called on main thread");
        uint64_t current_rendered = _render_fence->GetValue();
        frame_index++;

        if (current_rendered == presented_index) {
            return;
        }

        presented_index = current_rendered;

        auto* cmd_list = copy_cmd_list;
        cmd_list->Reset();
        Offset3D zero_offset = {0, 0, 0};

        RHIBlitTextureInfo blit_info;
        blit_info.filter = SF_CUBIC;

        cmd_list->BlitTexture(blit_info,
                              swapchain_textures[current_rendered % info.back_buffer_count],
                              present_texture);
        cmd_list->Close();

        RHISubmitInfo submit_info;
        submit_info.Wait(_render_fence, current_rendered);
        submit_info.Signal(present_fence, current_rendered);

        copy_queue->SubmitCommands(1, cmd_list, &submit_info);
    }

    void VirtualViewport::Impl::ResizeRenderThread(Moer::Vector2i extent) {
        // Implementation of ResizeRenderThread method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
    }

    VirtualViewportNextBackBufferInfo VirtualViewport::Impl::GetNextBackBuffer() {
        // Implementation of GetNextBackBuffer method
        // ...
        assert(Moer::IsCurrentlyGameThread());
        uint32_t backbuffer_index = frame_index % info.back_buffer_count;

        return {
            .backbuffer_index       = backbuffer_index,
            .backbuffer_ready_fence = present_fence};
    }

    RHIUnorderedAccessViewRef VirtualViewport::Impl::GetNextBackBufferUAV(uint32_t index) {
        // Implementation of GetNextBackBufferUAV method
        // ...
        assert(Moer::IsCurrentlyGameThread());

        return swapchain_uavs[index];
    }

}// namespace Moer
