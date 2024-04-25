#include "RenderResourceDeferred.h"
#include "log/LogSystem.h"
#include "rendergraph/RenderGraph.h"
#include "resources/AsyncResources.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

namespace Moer {

    struct RenderContext::Impl {
        Array<RenderGraph>             render_graphs;
        Array<RHIGraphicsCommandList*> cmd_lists;
        uint32_t                       frame_idx;
        RHICommandQueue*               gfx_queue;
        RHIFenceRef                    render_fence;

        Impl(RenderContextInitInfo _info) : frame_idx(0) {
            render_graphs.resize(_info.back_buffer_cnt);
            cmd_lists.resize(_info.back_buffer_cnt);

            for (uint32_t i = 0; i < _info.back_buffer_cnt; ++i) {

                auto* allocator = g_rhi->RHIGetCurrentCommandAllocator();
                cmd_lists[i]    = g_rhi->RHICreateGraphicsCommandList(allocator);
            }

            gfx_queue    = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);
            render_fence = g_rhi->RHICreateFence({EFenceUsageFlags::TIMELINE});
        }

        ~Impl() {
            for (auto* cmd_list : cmd_lists) {
                MoerDelete(cmd_list);
                cmd_list = nullptr;
            }
        }

        uint32_t GetFrameOffset() const {
            return frame_idx % cmd_lists.size();
        }

        RenderGraph& GetRenderGraph() {
            return render_graphs[GetFrameOffset()];
        }

        RHIGraphicsCommandList& GetCommandList() {
            return *cmd_lists[GetFrameOffset()];
        }

        void BeginFrame() {
            GetCommandList().Reset();
            GetRenderGraph().Reset();
            GetCommandList().BeginRecording();
        }

        void EndFrame(const VirtualViewport* _viewport) {
            auto& cmd_list = GetCommandList();
            cmd_list.EndRecording();
            RHISubmitInfo submit_info;
            auto*         back_buffer_fence = _viewport->GetBackBufferInfo().backbuffer_ready_fence;
            submit_info.Wait(render_fence, frame_idx);
            submit_info.Wait(back_buffer_fence, frame_idx);
            submit_info.Signal(render_fence, ++frame_idx);
            gfx_queue->SubmitCommands(1, &cmd_list, &submit_info);
        }

        void Present(VirtualViewport* _viewport) {
            _viewport->Present(render_fence);
        }

        RHICommandQueue* GetCommandQueue() {
            return gfx_queue;
        }
    };

    RenderContext::RenderContext() {
    }

    RenderContext::~RenderContext() {
        if (impl) {
            LOG_WARNING("RenderContext is not properly shut down.");
            MoerDelete(impl);
            impl = nullptr;
        }
    }

    void RenderContext::Init(RenderContextInitInfo _init_info) {
        impl = MoerNew(Impl)(_init_info);
    }

    void RenderContext::ShutDown() {
        if (impl) {
            MoerDelete(impl);
            impl = nullptr;
        }
    }

    void RenderContext::BeginFrame() {
        impl->BeginFrame();
    }

    uint32_t RenderContext::GetFrameOffset() const {
        return impl->GetFrameOffset();
    }
    uint32_t RenderContext::GetMaxFrameInFlight() const {
        return impl->cmd_lists.size();
    }

    RenderGraph& RenderContext::GetRenderGraph() {
        return impl->GetRenderGraph();
    }

    RHIGraphicsCommandList& RenderContext::GetCommandList() {
        return impl->GetCommandList();
    }

    void RenderContext::EndFrame(const VirtualViewport* _viewport) {
        impl->EndFrame(_viewport);
    }

    void RenderContext::Present(VirtualViewport* _viewport) {
        impl->Present(_viewport);
    }

    RHICommandQueue* RenderContext::GetCommandQueue() {
        return impl->GetCommandQueue();
    }

}// namespace Moer