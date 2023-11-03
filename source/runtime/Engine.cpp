#include "Engine.h"
#include "PixelFormat.h"
#include "imgui.h"
#include "log/LogSystem.h"

#include "Core.h"
#include "rhi/RHI.h"
#include "rhi/RHICommandQueue.h"
#include "rhi/RHICommandList.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderSystem.h"
#include "window/WindowContext.h"
#include <stdint.h>
namespace Moer {
    void Engine::Init(const EngineInitInfo& _info) {
        LOG_INFO("Engine Begin Initilization");

        InitCore();
        InitWindow();
        InitRenderSystem();

        LOG_INFO("Engine Initilization Finished");
    }

    void Engine::PostInit() {
        LOG_INFO("Engine Begin Post Init");

        LOG_INFO("Engine Post Init Finished");
    }

    void Engine::Run() {
        LOG_INFO("Engine Start Running");

        WindowContext& context = WindowContext::GetInstance();
        g_rhi->GUIInit(3);
        RHIGraphicsCommandList* gui_command_list = g_rhi->CreateGraphicsCommandList();

        RHICommandQueue* cmd_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);

        uint64_t    frame_index   = 0;
        RHIFenceRef present_fence = g_rhi->RHICreateFence(RHIFenceCreateInfo(EFenceUsage::PRESENT));
        while (!context.ShouldClose()) {
            RHIViewport* main_viewport   = g_rhi->RHIGetMainViewport();
            auto         next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);

            if (next_frame_info.backbuffer_index == UINT32_MAX) return;

            RHIUnorderedAccessView*              present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);
            RHIBarrierDependencyInfo             dependency_info;
            std::array<RHITextureBarrierInfo, 1> texture_barriers;

            texture_barriers[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
            texture_barriers[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED);
            texture_barriers[0].p_texture = present_view->GetTexture();
            texture_barriers[0].SetSrcStage(PS_BOTTOM_OF_PIPE);
            texture_barriers[0].SetDstStage(PS_FRAGMENT_SHADER);
            texture_barriers[0].SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED);
            texture_barriers[0].SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

            dependency_info.texture_barrier_count = 1;
            dependency_info.p_texture_barriers    = texture_barriers.data();

            gui_command_list->Reset();

            gui_command_list->Open();
            gui_command_list->SetPipelineBarrier(dependency_info);

            RHIRenderPassInfo pass_info;
            pass_info.color_attachments[0].color_attachment_action               = AC_CLEAR_STORE;
            pass_info.color_attachments[0].color_attachment_view.texture_view    = present_view;
            pass_info.color_attachments[0].color_attachment_view.required_layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;

            gui_command_list->BeginRenderPass(pass_info, "Imgui Window");

            g_rhi->GUIRender(ImGui::GetMainViewport()->DrawData, gui_command_list);

            RHIBarrierDependencyInfo             texture_dependency_info;
            std::array<RHITextureBarrierInfo, 1> texture_barriers_present;
            texture_barriers_present[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC);
            texture_barriers_present[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
            texture_barriers_present[0].p_texture = present_view->GetTexture();
            texture_barriers_present[0].SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

            texture_dependency_info.texture_barrier_count = 1;
            texture_dependency_info.p_texture_barriers    = texture_barriers.data();

            gui_command_list->SetPipelineBarrier(texture_dependency_info);

            gui_command_list->EndRenderPass();

            gui_command_list->Close();

            RHISubmitInfo submit_info{};

            //wait for last frame recording
            submit_info.Wait(present_fence, frame_index);
            //wait for back_buffer ready
            submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
            //signal this frame present fence
            submit_info.Signal(present_fence, ++frame_index);

            cmd_queue->SubmitCommands(1, gui_command_list);

            g_rhi->RHIPresentViewport(main_viewport, present_fence);
        }

        g_rhi->GUIShutDown();

        LOG_INFO("Engine Stop Running");
    }

    void Engine::InitCore() {
        Moer::TaskSystem::Init();
    }
    void Engine::ShutDownCore() {
        Moer::TaskSystem::ShutDown();
    }

    void Engine::InitRenderSystem() {

        RenderSystem::Init();
    }
    void Engine::ShutDownRenderSystem() {
        RenderSystem::ShutDown();
    }

    void Engine::InitWindow() {
        //todo: get from config
        SurfaceInfo info{"", 1920, 1080, "MoerEditor", false};
        WindowContext::GetInstance().Init(info);
    }
    void Engine::ShutDownWindow() {
    }
    void Engine::Tick() {
    }

    void Engine::Quit() {
        b_request_quiting = true;
        ShutDownRenderSystem();
        ShutDownWindow();
        ShutDownCore();

        SPDLOG_INFO("Engine Quit");
    }
}// namespace Moer
