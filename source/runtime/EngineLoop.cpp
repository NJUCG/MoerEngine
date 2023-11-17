// FILEPATH: /e:/GitHub/MoerEngine/source/runtime/EngineLoop.cpp

#include "EngineLoop.h"
#include "PixelFormat.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "window/WindowContext.h"
#include "rhi/RHI.h"
#include "rhi/RHICommandQueue.h"
#include "rhi/RHICommandList.h"

#include <imgui.h>
#define ENGINE_LOOP_NEW(target)    new target()
#define ENGINE_LOOP_DELETE(target) delete target
namespace Moer {
    struct EngineLoopData {

        ~EngineLoopData() {
            delete ui_command_list;
            delete command_queue;
        }
        RHITextureCreateInfo create_info;

        RHIGraphicsCommandList* ui_command_list = nullptr;
        RHICommandQueue*        command_queue   = nullptr;
        RHIFenceRef             present_fence   = nullptr;

        RHIShaderResourceViewRef main_render_view_copy    = nullptr;
        RHITextureRef            main_render_texture_copy = nullptr;

        uint64_t FrameIndex() const { return frame_index; }
        uint64_t IncrementFrameIndex() { return ++frame_index; }

    private:
        uint64_t frame_index = 0;
    };

    struct RenderThreadFence {
        RenderThreadFence(uint32_t _max_frame_in_flight) : max_frame_in_flight(_max_frame_in_flight),
                                                           max_lacking_frames(std::max(_max_frame_in_flight - 1, 0u)) {}
        uint64_t CurrentFrameIndex() const { return fences->GetValue(); }

        void WaitForFrame(uint64_t _current_frame) {
            if (fences->GetValue() + max_lacking_frames < _current_frame)
                fences->Wait(_current_frame);
        }
        RHIFenceRef fences;

        const uint32_t max_frame_in_flight = 3;
        const uint32_t max_lacking_frames  = 2;
    };

    void EngineLoop::Run() {
        ProcessInputEvents();
        RenderUI();
    }
    void EngineLoop::BeforeLoop() {
        // implementation of BeforeLoop method
    }
    void EngineLoop::Init() {

        data = ENGINE_LOOP_NEW(EngineLoopData);
        //to avoid ref counter become zero when first created
        data->present_fence   = g_rhi->RHICreateFence(RHIFenceCreateInfo(EFenceUsage::PRESENT));
        data->ui_command_list = g_rhi->CreateGraphicsCommandList(nullptr);
        data->command_queue   = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);

        data->create_info = RHITextureCreateInfo::Create("texture_copy", ETextureDimension::TEX_2D)
                                .SetArraySize(1)
                                .SetInitialLayout(TEXTURE_LAYOUT_UNDEFINED)
                                .SetFormat(PF_R8G8B8A8_SRGB)
                                .SetUAVFormat(PF_R8G8B8A8_SRGB)
                                .SetDepth(1)
                                .SetExtent({1, 1})
                                .SetNumMips(1)
                                .SetUsageFlags(ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED);
        //create default render texture
        data->main_render_texture_copy = g_rhi->RHICreateTexture(data->create_info);

        g_rhi->GUIInit(3);
    }

    void EngineLoop::AquireRenderThreadResult() {
        // implementation of AcquireRenderThreadResult method
    }
    void EngineLoop::RenderUI() {

        g_rhi->GUINewFrame();
        ImGui::NewFrame();
        {
            // OnDrawUI();
            for (auto& func : on_draw_ui_funcs) {
                func();
            }
        }
        ImGui::Render();

        ImDrawData* main_draw_data     = ImGui::GetDrawData();
        const bool  b_window_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

        if (!b_window_minimized) {
            RHIViewport* main_viewport   = g_rhi->RHIGetMainViewport();
            auto         present_fence   = data->present_fence;
            auto*        ui_command_list = data->ui_command_list;
            auto*        command_queue   = data->command_queue;

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
            present_fence->Wait(data->FrameIndex());

            ui_command_list->Reset();

            ui_command_list->Open();
            ui_command_list->SetPipelineBarrier(dependency_info);

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

            ui_command_list->BeginRenderPass(pass_info, "Imgui Window");

            auto* draw_data = ImGui::GetDrawData();

            g_rhi->GUIRender(main_draw_data, ui_command_list);

            ui_command_list->EndRenderPass();

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

            ui_command_list->SetPipelineBarrier(texture_dependency_info);

            ui_command_list->Close();

            RHISubmitInfo submit_info{};

            //wait for last frame recording(don't need if wait before reseting command list)
            submit_info.Wait(present_fence, data->FrameIndex());
            //wait for back_buffer ready
            submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
            //signal this frame present fence
            submit_info.Signal(present_fence, data->IncrementFrameIndex());

            command_queue->SubmitCommands(1, ui_command_list, &submit_info);

            g_rhi->RHIPresentViewport(main_viewport, present_fence);
        }
    }

    void EngineLoop::ProcessInputEvents() {
        //window io tick
        WindowContext::Tick();
    }

    bool EngineLoop::ShouldEndLoop() {
        // implementation of ShouldQuit method
        return WindowContext::ShouldClose(WindowContext::GetMainWindow());
    }

    void EngineLoop::AfterLoop() {
        // implementation of AfterLoop method
        ENGINE_LOOP_DELETE(data);
        g_rhi->GUIShutDown();
    }

    void EngineLoop::RegisterOnDrawUI(std::function<void()> _func) {
        // implementation of RegisterOnDrawUI method
        on_draw_ui_funcs.emplace_back(_func);
    }
}// namespace Moer