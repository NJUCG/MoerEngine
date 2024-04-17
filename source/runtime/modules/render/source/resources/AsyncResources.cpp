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
#include <cstdint>
namespace Moer {
    class VirtualViewport::Impl {

    public:
        Impl(const VirtualViewportCreateInfo& create_info);
        ~Impl();
        //call on main thread
        void OnResize(Moer::Vector2i extent);

        //call from render thread
        VirtualViewportBackBufferInfo GetBackBufferInfo() const;
        Extent3D                      GetBackBufferExtent();
        RHIUAVRef                     GetDepthBufferUav();
        RHITextureRef                 GetDepthTexture();

        void Present(RHIFenceRef _render_fence);

        const VirtualViewportInfo& GetInfo() const { return info; }

        RHISRVRef GetPresentTextureSRV() { return present_texture_srv; }

    private:
        friend VirtualViewport;
        void InitRenderThread();
        void CreateResources();
        void ResizeRenderThread(Moer::Vector2i extent);

// in Application mode, Present operations happens on render thread
#if !defined(EDITOR_MODE_ON)
        RHIViewportRef viewport;
#endif
        VirtualViewportInfo info;

    private:
        RHITextureCreateInfo upload_texture_create_info;
        RHITextureCreateInfo depth_texture_create_info;

        RHIFenceRef present_fence;

        RHITextureRef present_texture;
        RHISRVRef     present_texture_srv;

        Moer::Array<RHITextureRef> swapchain_textures;
        Moer::Array<RHIUAVRef>     swapchain_uavs;
        uint64_t                   frame_index     = 0;
        uint64_t                   presented_index = 0;

        RHITextureRef depth_texture;
        RHIUAVRef     depth_texture_uav;

        RHICommandQueue* copy_queue;

        RHIGraphicsCommandList* copy_cmd_list;
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

    Extent3D VirtualViewport::GetBackBufferExtent() {
        return impl->GetBackBufferExtent();
    }

    VirtualViewportBackBufferInfo VirtualViewport::GetBackBufferInfo() const {
        return impl->GetBackBufferInfo();
    }

    RHISRV* VirtualViewport::GetPresentTextureSRV() {
        return impl->GetPresentTextureSRV();
    }

    VirtualViewport::Impl::~Impl() {
    }
    VirtualViewport::Impl::Impl(const VirtualViewportCreateInfo& create_info) {

        present_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});

        copy_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);
        copy_cmd_list = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());

        info.back_buffer_count = create_info.back_buffer_count;
        info.extent            = create_info.extent;
        info.format            = create_info.format;
        info.name              = create_info.name;

        upload_texture_create_info = RHITextureCreateInfo::Create2D("virtual viewport",
                                                                    create_info.extent,
                                                                    EPixelFormat::PF_B8G8R8A8_UNORM)
                                         .SetArraySize(1)
                                         .SetNumMips(1)
                                         .SetClearAttachment({})
                                         .SetUsageFlags(
                                             //   ETextureUsageFlags::COLOR_ATTACHMENT |
                                             ETextureUsageFlags::TRANSFER_SRC |
                                             ETextureUsageFlags::UNORDERED_ACCESS
                                             //|
                                             //     ETextureUsageFlags::SRGB |
                                             //    ETextureUsageFlags::SAMPLED
                                         );

        depth_texture_create_info = RHITextureCreateInfo::Create2D("virtual viewport depth",
                                                                   create_info.extent,
                                                                   EPixelFormat::PF_D32_SFLOAT_S8_UINT)
                                        .SetArraySize(1)
                                        .SetNumMips(1)
                                        .SetClearAttachment({})
                                        .SetUsageFlags(
                                            ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED);

        CreateResources();
        copy_queue->WaitForQueueComplete();
    }
    void VirtualViewport::Impl::CreateResources() {

        auto present_texture_create_info = upload_texture_create_info;

        present_texture     = g_rhi->RHICreateTexture(present_texture_create_info.SetUsageFlags(
            ETextureUsageFlags::COLOR_ATTACHMENT |
            //  ETextureUsageFlags::UNORDERED_ACCESS |
            ETextureUsageFlags::SAMPLED |
            ETextureUsageFlags::TRANSFER_DST));
        present_texture_srv = g_rhi->RHICreateTextureSRV(present_texture, EPixelFormat::PF_B8G8R8A8_UNORM);

        swapchain_textures.resize(info.back_buffer_count);
        swapchain_uavs.resize(info.back_buffer_count);
        for (int i = 0; i < info.back_buffer_count; ++i) {
            swapchain_textures[i] = g_rhi->RHICreateTexture(upload_texture_create_info);
            swapchain_uavs[i] =
                g_rhi->RHICreateTextureUAV(swapchain_textures[i],
                                           EPixelFormat::PF_B8G8R8A8_UNORM);
        }

        depth_texture     = g_rhi->RHICreateTexture(depth_texture_create_info);
        depth_texture_uav = g_rhi->RHICreateTextureUAV(depth_texture, depth_texture_create_info.format);

        RHIFenceRef fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::BINARY});

        RHIBarrierDependencyInfo barrier_info;
        auto&                    barriers = barrier_info.texture_barriers;
        barriers.resize(info.back_buffer_count + 2);

        for (uint32_t i = 0; i < info.back_buffer_count; ++i) {
            barriers[i].SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC);
            barriers[i].SetTexture(swapchain_textures[i]);
            barriers[i].SetSubResourceRange({});
            barriers[i].SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED);
        }
        barriers[info.back_buffer_count].SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barriers[info.back_buffer_count].SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED);
        barriers[info.back_buffer_count].SetTexture(present_texture);
        barriers[info.back_buffer_count].SetSubResourceRange({});

        barriers[info.back_buffer_count + 1].SetDstTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE);
        barriers[info.back_buffer_count + 1].SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED);
        barriers[info.back_buffer_count + 1].SetDstStage(PS_EARLY_FRAGMENT_TESTS);
        barriers[info.back_buffer_count + 1].SetTexture(depth_texture);
        barriers[info.back_buffer_count + 1].SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE));

        copy_cmd_list->BeginRecording();
        copy_cmd_list->SetPipelineBarrier(barrier_info);
        copy_cmd_list->EndRecording();
        RHISubmitInfo submit_info;
        submit_info.Signal(fence, 1);
        copy_queue->SubmitCommands(1, copy_cmd_list, &submit_info);
    }
    void VirtualViewport::Impl::InitRenderThread() {
        // Implementation of InitRenderThread method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
    }

    void VirtualViewport::Impl::OnResize(Moer::Vector2i extent) {

        assert(Moer::IsCurrentlyRenderThread() && "OnResize should be called on main thread");

        if (extent == info.extent) {
            return;
        }
        info.extent = extent;
        upload_texture_create_info.SetExtent(extent);
        depth_texture_create_info.SetExtent(extent);
        depth_texture_create_info.SetExtent(extent);
        copy_queue->WaitForQueueComplete();
        copy_cmd_list->Reset();
        CreateResources();
        copy_queue->WaitForQueueComplete();
    }

    void VirtualViewport::Impl::Present(RHIFenceRef _render_fence) {
        // Implementation of Present method
        // ...
        assert(Moer::IsCurrentlyRenderThread() && "Present should be called on main thread");
        uint64_t current_rendered = _render_fence->GetValue();
        frame_index++;

        presented_index = frame_index;

        auto* cmd_list = copy_cmd_list;
        present_fence->Wait(presented_index - 1);
        cmd_list->Reset();
        Offset3D zero_offset = {0, 0, 0};

        Extent3D src_extent = swapchain_textures[current_rendered % info.back_buffer_count]->GetExtent3D();

        Offset2D           extent = {upload_texture_create_info.extent.x, upload_texture_create_info.extent.y};
        RHIBlitTextureInfo blit_info;
        blit_info.filter         = SF_CUBIC;
        blit_info.src_offsets[0] = zero_offset;
        blit_info.src_offsets[1] = Offset3D(src_extent.x, src_extent.y, 1);
        blit_info.dst_offsets[0] = zero_offset;
        blit_info.dst_offsets[1] = Offset3D(extent.x, extent.y, 1);
        blit_info.src_layout     = TEXTURE_LAYOUT_TRANSFER_SRC;
        blit_info.dst_layout     = TEXTURE_LAYOUT_TRANSFER_DST;
        blit_info.src_slice      = {};
        blit_info.dst_slice      = {};

        RHIBarrierDependencyInfo barrier_info{};
        auto&                    barriers      = barrier_info.texture_barriers;
        uint32_t                 present_index = presented_index % info.back_buffer_count;
        barriers.emplace_back(RHITextureBarrierInfo::Create()
                                  .SetTexture(present_texture)
                                  .SetSrcTextureLayout(present_texture->GetLayout({ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1}))
                                  .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_DST)
                                  .SetSubResourceRange({}))
            .SetSrcQueueType(ECommandQueueType::GRAPHICS)
            .SetDstQueueType(ECommandQueueType::GRAPHICS);
        present_texture->SetLayout({ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1}, TEXTURE_LAYOUT_TRANSFER_DST);
        // barriers.emplace_back(RHITextureBarrierInfo::Create()
        //                           .SetTexture(swapchain_textures[present_index])
        //                           .SetSrcTextureLayout(swapchain_textures[present_index]->GetLayout({ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1}))
        //                           .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC));

        swapchain_textures[present_index]->SetLayout({ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1}, TEXTURE_LAYOUT_TRANSFER_SRC);
        barriers[0]
            .SetSrcAccessFlags(ERHIAccessFlags::SHADER_SAMPLED_READ)
            .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetSrcStage(PS_FRAGMENT_SHADER)
            .SetDstStage(PS_TRANSFER);
        // barriers[1]
        //     .SetSrcAccessFlags(ERHIAccessFlags::SHADER_SAMPLED_READ)
        //     .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
        //     .SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT)
        //     .SetDstStage(PS_TRANSFER);

        cmd_list->BeginRecording();
        cmd_list->SetPipelineBarrier(barrier_info);
        cmd_list->BlitTexture(blit_info,
                              swapchain_textures[present_index],
                              present_texture);

        barriers[0]
            .SetSrcTextureLayout(TEXTURE_LAYOUT_TRANSFER_DST)
            .SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetDstAccessFlags(ERHIAccessFlags::SHADER_SAMPLED_READ)
            .SetDstStage(PS_FRAGMENT_SHADER)
            .SetSrcStage(PS_TRANSFER);
        barrier_info.texture_barriers.resize(1);
        present_texture->SetLayout({ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1}, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cmd_list->SetPipelineBarrier(barrier_info);
        cmd_list->EndRecording();

        RHISubmitInfo submit_info;
        submit_info.Wait(_render_fence, presented_index);
        submit_info.Signal(present_fence, presented_index);

        copy_queue->SubmitCommands(1, cmd_list, &submit_info);
    }

    void VirtualViewport::Impl::ResizeRenderThread(Moer::Vector2i extent) {
        // Implementation of ResizeRenderThread method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
    }

    VirtualViewportBackBufferInfo VirtualViewport::Impl::GetBackBufferInfo() const {
        // Implementation of GetNextBackBuffer method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
        uint32_t backbuffer_index = frame_index % info.back_buffer_count;

        return {
            .back_buffer_index      = backbuffer_index,
            .backbuffer_ready_fence = present_fence,
            .backbuffer_uav         = swapchain_uavs[backbuffer_index],
        };
    }
    Extent3D VirtualViewport::Impl::GetBackBufferExtent() {
        auto      info = GetBackBufferInfo();
        RHIUAVRef uav  = GetBackBufferInfo().backbuffer_uav;
        return uav->GetTexture()->GetExtent3D();
    }

    RHIUAVRef VirtualViewport::Impl::GetDepthBufferUav() {
        // Implementation of GetDepthBufferUAV method
        // ...
        assert(Moer::IsCurrentlyRenderThread());
        return depth_texture_uav;
    }

    RHITextureRef VirtualViewport::Impl::GetDepthTexture() {
        // Implementation of getDepthTexture method
        // ...
        assert(Moer::IsCurrentlyRenderThread());

        return depth_texture;
    }

}// namespace Moer