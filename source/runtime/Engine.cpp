#include "Engine.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
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
#include "ui/UIBase.h"
#include "window/WindowContext.h"
#include <filesystem>
#include <stdint.h>
namespace Moer {
    void Engine::Init(const EngineInitInfo& _info) {
        LOG_INFO("Engine Begin Initilization");

        InitCore(_info.workspace_path);
        InitRenderSystem();
        InitWindow();

        LOG_INFO("Engine Initilization Finished");
    }

    void Engine::PostInit() {
        LOG_INFO("Engine Begin Post Init");
        PostInitRenderSystem();
        LOG_INFO("Engine Post Init Finished");
    }
    void Engine::Run() {
        LOG_INFO("Engine Start Running");
        TestDrawUI();
        LOG_INFO("Engine Stop Running");
    }

    void Engine::InitCore(const std::filesystem::path& workspace_path) {
        Moer::ConfigManager::GetInstance().Init(workspace_path);
        Moer::TaskSystem::Init();
        Moer::LogSystem::Init();
    }
    void Engine::ShutDownCore() {
        Moer::TaskSystem::ShutDown();
    }

    void Engine::InitRenderSystem() {

        RenderSystem::Init();
    }
    void Engine::PostInitRenderSystem() {
        RenderSystem::PostInit();
    }
    void Engine::ShutDownRenderSystem() {
        RenderSystem::ShutDown();
    }

    void Engine::InitWindow() {
        //todo: get from config
        SurfaceInfo info{"", 1920, 1080, "MoerEditor", false};
        WindowContext::Init(info);
    }
    void Engine::ShutDownWindow() {
        WindowContext::ShutDown();
    }
    void Engine::Tick() {
        TestDrawUI();
    }

    void Engine::Quit() {
        b_request_quiting = true;
        ShutDownRenderSystem();
        ShutDownWindow();
        ShutDownCore();

        SPDLOG_INFO("Engine Quit");
    }

    void Engine::RegisterOnDrawUI(std::function<void()> _func) {
        on_draw_ui_funcs.push_back(_func);
    }

    void Engine::OnDrawUI() {
        for (auto& func : on_draw_ui_funcs) {
            func();
        }
    }

    void Engine::TestDrawUI() {

        g_rhi->GUIInit(3);

        RHIGraphicsCommandList* gui_command_list = g_rhi->CreateGraphicsCommandList();

        RHICommandQueue* cmd_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);

        uint64_t    frame_index   = 0;
        RHIFenceRef present_fence = g_rhi->RHICreateFence(RHIFenceCreateInfo(EFenceUsage::PRESENT));
        while (!WindowContext::ShouldClose(WindowContext::GetMainWindow())) {

            //window io tick
            WindowContext::Tick();

            g_rhi->GUINewFrame();
            ImGui::NewFrame();
            {
                OnDrawUI();
            }
            ImGui::Render();

            ImDrawData* main_draw_data     = ImGui::GetDrawData();
            const bool  b_window_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

            if (!b_window_minimized) {
                RHIViewport* main_viewport = g_rhi->RHIGetMainViewport();

                auto next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);

                if (next_frame_info.backbuffer_index == UINT32_MAX) return;

                RHIUnorderedAccessView*              present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);
                RHIBarrierDependencyInfo             dependency_info;
                std::array<RHITextureBarrierInfo, 1> texture_barriers;

                texture_barriers[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
                texture_barriers[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED);
                texture_barriers[0].p_texture = present_view->GetTexture();
                texture_barriers[0].SetSrcStage(PS_BOTTOM_OF_PIPE);
                texture_barriers[0].SetDstStage(PS_COLOR_ATTACHMENT_OUTPUT);
                texture_barriers[0].SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED);
                texture_barriers[0].SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

                dependency_info.texture_barrier_count = 1;
                dependency_info.p_texture_barriers    = texture_barriers.data();

                //wait for last frame gui_command_list submission
                present_fence->Wait(frame_index);

                gui_command_list->Reset();

                gui_command_list->Open();
                gui_command_list->SetPipelineBarrier(dependency_info);

                RHIRenderPassInfo pass_info;
                pass_info.color_attachments[0].color_attachment_action               = AC_CLEAR_STORE;
                pass_info.color_attachments[0].color_attachment_view.texture_view    = present_view;
                pass_info.color_attachments[0].color_attachment_view.required_layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;

                pass_info.color_attachments[0].color_attachment_view.clear_attachment.value.color = {0.0f, 0.0f, 0.0f, 1.0f};

                auto viewport_extent                = main_viewport->GetViewportExtent();
                pass_info.render_area.offset.x      = 0;
                pass_info.render_area.offset.y      = 0;
                pass_info.render_area.extent.width  = viewport_extent.width;
                pass_info.render_area.extent.height = viewport_extent.height;

                gui_command_list->BeginRenderPass(pass_info, "Imgui Window");

                auto* draw_data = ImGui::GetDrawData();

                g_rhi->GUIRender(main_draw_data, gui_command_list);

                gui_command_list->EndRenderPass();

                RHIBarrierDependencyInfo             texture_dependency_info;
                std::array<RHITextureBarrierInfo, 1> texture_barriers_present;
                texture_barriers_present[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC);
                texture_barriers_present[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
                texture_barriers_present[0].p_texture = present_view->GetTexture();
                texture_barriers_present[0].SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);
                texture_barriers_present[0].SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT);
                texture_barriers_present[0].SetDstStage(PS_NONE);

                texture_dependency_info.texture_barrier_count = 1;
                texture_dependency_info.p_texture_barriers    = texture_barriers_present.data();

                gui_command_list->SetPipelineBarrier(texture_dependency_info);

                gui_command_list->Close();

                RHISubmitInfo submit_info{};

                //wait for last frame recording(don't need if wait before reseting command list)
                submit_info.Wait(present_fence, frame_index);
                //wait for back_buffer ready
                submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
                //signal this frame present fence
                submit_info.Signal(present_fence, ++frame_index);

                cmd_queue->SubmitCommands(1, gui_command_list, &submit_info);

                g_rhi->RHIPresentViewport(main_viewport, present_fence);
            }
            WindowContext::PollEvents();
        }

        g_rhi->GUIShutDown();
    }
}// namespace Moer
