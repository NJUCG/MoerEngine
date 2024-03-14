#include "ImGUIRenderer.h"
#include "IconsFontAwesome6.h"
#include "RenderThread.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"

#include "math/Math.h"
#include "misc/STL.h"

#include "taskgraph/GraphTask.h"
#include "taskgraph/ThreadManager.h"
#include "window/WindowContext.h"

#include <atomic>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>

struct UIFrameData {

    RHIGraphicsCommandList* ui_command_list = nullptr;
    RHICommandQueue*        command_queue   = nullptr;
    RHIFenceRef             present_fence   = nullptr;

    uint64_t timeline_index = 0;

    ~UIFrameData() {
        MoerDelete(ui_command_list);
        MoerDelete(command_queue);
    }
};
struct GuiFrameRenderBuffers {

    RHIBufferRef vertex_buffer;
    RHIBufferRef index_buffer;

    RHIBufferRef staging_vertex_buffer;
    RHIBufferRef staging_index_buffer;
};
struct GuiBackendData {
    size_t buffer_memory_alignment;

    RHIGraphicsPipelineStateRef pipeline;
    RHIShaderRef                shader_module_vert;
    RHIShaderRef                shader_module_frag;

    // Font data
    RHISamplerRef font_sampler;
    RHITextureRef font_texture;
    RHISRVRef     font_view;
    RHIBufferRef  upload_buffer;

    // Render buffers for main window
    GuiFrameRenderBuffers* main_viewport_render_buffers;
    RHIViewport*           main_viewport;

    EPixelFormat attachment_format;
    uint32_t     num_frames_in_flight;

    GuiBackendData() {
        memset((void*)this, 0, sizeof(*this));
        buffer_memory_alignment = 256;
    }
    ~GuiBackendData();
};

class ImGuiShaderVert : public Shader {
    DEFINE_SHADER_TYPE(ImGuiShaderVert, Global, RENDER_API, ...)
public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UIVertex)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, mvp)
    END_SHADER_CONSTANT_STRUCT_DEFINITION()
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    // DEFINE_SHADER_PARAM_CBV(Constant, vertexBuffer)
    DEFINE_SHADER_PARAM_STRUCT(UIVertex, vertexBuffer)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
IMPLEMENT_SHADER_TYPE(ImGuiShaderVert, "GuiVert.vert", "main", ST_VERTEX)
class ImGuiShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(ImGuiShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_SAMPLER(Sampler, sampler0)
    DEFINE_SHADER_PARAM_SRV(Texture2D, texture0)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(ImGuiShaderFrag, "GuiFrag.frag", "main", ST_FRAGMENT)

class ImGUIRenderer::Impl {
public:
    Impl() {
    }
    ~Impl() {
    }

    void Init();
    void ShutDown();

    void BeginRenderFrame();
    void EndRenderFrame();

private:
    RHIGraphicsCommandList* ui_command_list = nullptr;
    RHICommandQueue*        command_queue   = nullptr;
    RHIFenceRef             present_fence   = nullptr;

    uint64_t timeline_index = 0;

    //render_thread
    RHIViewportNextBackBufferInfo next_frame_info;
};
void GuiInitPlatformInterface();
bool CreateDeviceObjects();
void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);
void InvalidateDeviceObjects();

void GUIUploadData(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);
void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);

void GuiRenderWindow(ImGuiViewport* viewport, void*);
void GuiSwapbuffer(ImGuiViewport* viewport, void*);

inline GuiBackendData* GetBackendData() {
    return ImGui::GetCurrentContext() ? (GuiBackendData*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

struct GuiViewportData {

    RHICommandQueue*        command_queue;
    RHIGraphicsCommandList* comand_list;

    RHIGraphicsCommandList* upload_command_list;

    RHIFenceRef present_fence;

    RHIViewportRef viewport;

    Moer::Array<GuiFrameRenderBuffers> render_buffers;// Used by all viewports

    RHIViewportNextBackBufferInfo next_frame_info;

    uint64_t        frame_index;
    uint32_t        viewport_index;
    static uint32_t viewport_count;

    GuiViewportData(uint32_t _frame_in_flight) {
        memset((void*)this, 0, sizeof(*this));
        render_buffers.resize(_frame_in_flight);
        for (uint32_t i = 0; i < _frame_in_flight; ++i) {
            render_buffers[i].vertex_buffer         = nullptr;
            render_buffers[i].index_buffer          = nullptr;
            render_buffers[i].staging_vertex_buffer = nullptr;
            render_buffers[i].staging_index_buffer  = nullptr;
        }
        viewport_index = viewport_count;
        viewport_count++;
    }
    ~GuiViewportData() {
        viewport_count--;
    }
};

void ImGUIRenderer::Init() {
    impl = MoerNew(Impl)();
    impl->Init();
}

void ImGUIRenderer::ShutDown() {
    impl->ShutDown();
    MoerDelete(impl);
}

void ImGUIRenderer::BeginRenderFrame() {
    impl->BeginRenderFrame();
}
//collect render data and record
void ImGUIRenderer::EndRenderFrame() {
    impl->EndRenderFrame();
}

void ImGUIRenderer::Impl::Init() {
    present_fence   = g_rhi->RHICreateFence(RHIFenceCreateInfo(EFenceUsageFlags::PRESENT));
    ui_command_list = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());
    command_queue   = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

    ImGuiIO& io = ImGui::GetIO();
    assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

    const Moer::ConfigManager& config_manager      = Moer::ConfigManager::GetInstance();
    uint32_t                   max_frame_in_flight = config_manager.GetInitConfig().max_frame_in_flight;

    GuiBackendData* render_backend_data       = MoerNew(GuiBackendData)();
    render_backend_data->num_frames_in_flight = max_frame_in_flight;

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiViewport*   main_viewport  = ImGui::GetMainViewport();
    GuiViewportData* viewport_data  = MoerNew(GuiViewportData)(max_frame_in_flight);
    main_viewport->RendererUserData = viewport_data;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        GuiInitPlatformInterface();
}

void ImGUIRenderer::Impl::BeginRenderFrame() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call GuiInit(uint32_t)?");

    if (!bd->pipeline)
        CreateDeviceObjects();
    ImGui::NewFrame();
}

void ImGUIRenderer::Impl::EndRenderFrame() {
    ImGui::Render();

    ImDrawData* main_draw_data     = ImGui::GetDrawData();
    const bool  b_window_minimized = main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f;

    if (!b_window_minimized) {

        RHIViewport* main_viewport = g_rhi->RHIGetMainViewport();

        {
            EnqueueRenderTask([main_viewport, this] {
                next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);
            });
            // auto next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);

            // if (next_frame_info.backbuffer_index == UINT32_MAX) return;
            EnqueueRenderTask([main_viewport, this] {
                if (next_frame_info.backbuffer_index == UINT32_MAX) return;
                RHIUAV*                  present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);
                RHIBarrierDependencyInfo dependency_info;
                auto&                    texture_barriers = dependency_info.texture_barriers;
                texture_barriers.resize(1);

                texture_barriers[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
                texture_barriers[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED);
                texture_barriers[0].SetTexture(present_view->GetTexture());
                texture_barriers[0].SetSrcStage(PS_BOTTOM_OF_PIPE);
                texture_barriers[0].SetDstStage(PS_COLOR_ATTACHMENT_OUTPUT);
                texture_barriers[0].SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED);
                texture_barriers[0].SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

                //wait for last frame gui_command_list submission

                present_fence->Wait(timeline_index);

                ui_command_list->Reset();

                ui_command_list->BeginRecording();
                ui_command_list->SetPipelineBarrier(dependency_info);
            });

            auto* draw_data = ImGui::GetDrawData();

            GUIUploadData(draw_data, ui_command_list, &next_frame_info);

            EnqueueRenderTask([this, main_viewport] {
                if (next_frame_info.backbuffer_index == UINT32_MAX) return;
                RHIUAV* present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);

                RHIRenderPassInfo pass_info{};
                pass_info.color_attachments[0].color_attachment_action               = AC_CLEAR_STORE;
                pass_info.color_attachments[0].color_attachment_view.texture_view    = present_view;
                pass_info.color_attachments[0].color_attachment_view.required_layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;

                pass_info.color_attachments[0].color_attachment_view.clear_attachment = RHIClearAttachment();

                auto viewport_extent                = main_viewport->GetViewportExtent();
                pass_info.render_area.offset.x      = 0;
                pass_info.render_area.offset.y      = 0;
                pass_info.render_area.extent.width  = viewport_extent.width;
                pass_info.render_area.extent.height = viewport_extent.height;

                ui_command_list->BeginRenderPass(pass_info, "Imgui Window");
            });

            GUIRender(main_draw_data, ui_command_list, &next_frame_info);

            EnqueueRenderTask([this, main_viewport] {
                if (next_frame_info.backbuffer_index == UINT32_MAX) return;
                RHIUAV* present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);

                ui_command_list->EndRenderPass();

                RHIBarrierDependencyInfo texture_dependency_info;
                auto&                    texture_barriers_present = texture_dependency_info.texture_barriers;
                texture_barriers_present.resize(1);

                texture_barriers_present[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC);
                texture_barriers_present[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
                texture_barriers_present[0].SetTexture(present_view->GetTexture());
                texture_barriers_present[0].SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);
                texture_barriers_present[0].SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT);
                texture_barriers_present[0].SetDstStage(PS_NONE);

                ui_command_list->SetPipelineBarrier(texture_dependency_info);

                ui_command_list->EndRecording();

                RHISubmitInfo submit_info{};

                //wait for last frame recording(don't need if wait before reseting command list)
                submit_info.Wait(present_fence, timeline_index);
                //wait for back_buffer ready
                submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
                //signal this frame present fence
                submit_info.Signal(present_fence, ++timeline_index);

                command_queue->SubmitCommands(1, ui_command_list, &submit_info);

                g_rhi->RHIPresentViewport(main_viewport, present_fence);
            });
        }
        // auto next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);

        // if (next_frame_info.backbuffer_index == UINT32_MAX) return;

        // RHIUnorderedAccessView*  present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);
        // RHIBarrierDependencyInfo dependency_info;
        // auto&                    texture_barriers = dependency_info.texture_barriers;
        // texture_barriers.resize(1);

        // texture_barriers[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
        // texture_barriers[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED);
        // texture_barriers[0].SetTexture(present_view->GetTexture());
        // texture_barriers[0].SetSrcStage(PS_BOTTOM_OF_PIPE);
        // texture_barriers[0].SetDstStage(PS_COLOR_ATTACHMENT_OUTPUT);
        // texture_barriers[0].SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED);
        // texture_barriers[0].SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

        // //wait for last frame gui_command_list submission

        // present_fence->Wait(timeline_index);

        // ui_command_list->Reset();

        // ui_command_list->BeginRecording();
        // ui_command_list->SetPipelineBarrier(dependency_info);

        // RHIRenderPassInfo pass_info{};
        // pass_info.color_attachments[0].color_attachment_action               = AC_CLEAR_STORE;
        // pass_info.color_attachments[0].color_attachment_view.texture_view    = present_view;
        // pass_info.color_attachments[0].color_attachment_view.required_layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;

        // pass_info.color_attachments[0].color_attachment_view.clear_attachment = RHIClearAttachment();

        // auto viewport_extent                = main_viewport->GetViewportExtent();
        // pass_info.render_area.offset.x      = 0;
        // pass_info.render_area.offset.y      = 0;
        // pass_info.render_area.extent.width  = viewport_extent.width;
        // pass_info.render_area.extent.height = viewport_extent.height;
        // auto* draw_data                     = ImGui::GetDrawData();

        // GUIUploadData(draw_data, ui_command_list);
        // ui_command_list->BeginRenderPass(pass_info, "Imgui Window");

        // GUIRender(main_draw_data, ui_command_list);

        // ui_command_list->EndRenderPass();

        // RHIBarrierDependencyInfo texture_dependency_info;
        // auto&                    texture_barriers_present = texture_dependency_info.texture_barriers;
        // texture_barriers_present.resize(1);

        // texture_barriers_present[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC);
        // texture_barriers_present[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
        // texture_barriers_present[0].SetTexture(present_view->GetTexture());
        // texture_barriers_present[0].SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);
        // texture_barriers_present[0].SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT);
        // texture_barriers_present[0].SetDstStage(PS_NONE);

        // ui_command_list->SetPipelineBarrier(texture_dependency_info);

        // ui_command_list->EndRecording();

        // RHISubmitInfo submit_info{};

        // //wait for last frame recording(don't need if wait before reseting command list)
        // submit_info.Wait(present_fence, timeline_index);
        // //wait for back_buffer ready
        // submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
        // //signal this frame present fence
        // submit_info.Signal(present_fence, ++timeline_index);

        // command_queue->SubmitCommands(1, ui_command_list, &submit_info);

        // g_rhi->RHIPresentViewport(main_viewport, present_fence);
    }
    {
        auto& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
        }
        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

        if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiRenderWindow(platform_io.Viewports[i], nullptr);
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiSwapbuffer(platform_io.Viewports[i], nullptr);
        }
    }
}

void ImGUIRenderer::Impl::ShutDown() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    // Clean up windows and device objects
    ImGui::DestroyPlatformWindows();
    InvalidateDeviceObjects();

    io.BackendRendererName     = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);
    MoerDelete(bd);

    EnqueueRenderTask([this] {
        command_queue->WaitForQueueComplete();
        MoerDelete(command_queue);
        MoerDelete(ui_command_list);
    });

    Moer::RenderThreadFence fence;
    fence.BeginFence();
    fence.Wait();

    // Manually delete main viewport render resources in-case we haven't initialized for viewports
    auto* main_viewport = ImGui::GetDrawData()->OwnerViewport;
    if (GuiViewportData* viewport_data = (GuiViewportData*)main_viewport->RendererUserData) {
        // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
        for (uint32_t i = 0; i < bd->num_frames_in_flight; i++)
            DestroyRenderBuffers(&viewport_data->render_buffers[i]);
        MoerDelete(viewport_data);
        main_viewport->RendererUserData = nullptr;
    }
}

uint32_t GuiViewportData::viewport_count = 0;

GuiBackendData::~GuiBackendData() {
}

bool CreateDeviceObjects();
void CreateFontsTexture();

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiViewportData* _viewport_data, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread);

void GUIUploadData(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
    ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;
    // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);
    uint32_t total_size_vert = draw_data->TotalVtxCount;
    uint32_t total_size_idx  = draw_data->TotalIdxCount * sizeof(ImDrawIdx);

    GuiBackendData* backend_data = GetBackendData();

    GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

    uint32_t num_frames_in_flight = backend_data->num_frames_in_flight;
    EnqueueRenderTask([_next_frame_info_render_thread, viewport_data, num_frames_in_flight, total_size_vert, total_size_idx] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

        GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];

        if (render_buffers->vertex_buffer == nullptr || render_buffers->vertex_buffer->GetNumElement() < total_size_vert) {
            //delete the old one and create new
            if (render_buffers->vertex_buffer != nullptr) {}
            // render_buffers->vertex_buffer->DeRef();
            uint32_t new_size             = 4096 + total_size_vert;
            render_buffers->vertex_buffer = g_rhi->RHICreateBuffer<ImDrawVert>(
                new_size * sizeof(ImDrawVert),
                EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);

            render_buffers->staging_vertex_buffer = g_rhi->RHICreateBuffer<ImDrawVert>(
                new_size * sizeof(ImDrawVert), EBufferUsageFlags::CPU_VISIBLE);

            render_buffers->staging_vertex_buffer->SetName("staging_vertex_buffer");
        }
        if (render_buffers->index_buffer == nullptr || render_buffers->index_buffer->GetNumElement() < total_size_idx) {

            if (render_buffers->index_buffer != nullptr) {}
            uint32_t new_size            = 8192 + total_size_idx;
            render_buffers->index_buffer = g_rhi->RHICreateBuffer<ImDrawIdx>(
                new_size * sizeof(ImDrawIdx),
                EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::TRANSFER_DST);

            render_buffers->staging_index_buffer = g_rhi->RHICreateBuffer<ImDrawIdx>(
                new_size * sizeof(ImDrawIdx), EBufferUsageFlags::CPU_VISIBLE);
        }
    });

    ImDrawVert* vertex_dst = nullptr;
    ImDrawIdx*  index_dst  = nullptr;

    // RHIBufferRef staging_index_buffer  = render_buffers->staging_index_buffer;
    // RHIBufferRef staging_vertex_buffer = render_buffers->staging_vertex_buffer;

    // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(render_buffers->staging_vertex_buffer, 0, UINT64_MAX);
    // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(render_buffers->staging_index_buffer, 0, UINT64_MAX);
    // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
    // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
    size_t vertex_offset = 0;
    size_t index_offset  = 0;
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList*       cmd_list = draw_data->CmdLists[n];
        Moer::Array<ImDrawVert> vertices(cmd_list->VtxBuffer.Size);
        Moer::Array<ImDrawIdx>  indices(cmd_list->IdxBuffer.Size);
        memcpy(vertices.data(), cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(indices.data(), cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        EnqueueRenderTask([_next_frame_info_render_thread,
                           num_frames_in_flight,
                           vertices{std::move(vertices)},
                           indices{std::move(indices)},
                           viewport_data,
                           vertex_offset,
                           index_offset]() {
            if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
            GuiFrameRenderBuffers* render_buffers        = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];
            RHIBufferRef           staging_vertex_buffer = render_buffers->staging_vertex_buffer;
            RHIBufferRef           staging_index_buffer  = render_buffers->staging_index_buffer;

            ImDrawVert* vertex_dst = nullptr;
            ImDrawIdx*  index_dst  = nullptr;

            vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
            index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
            // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
            // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
            memcpy(vertex_dst + vertex_offset, vertices.data(), vertices.size() * sizeof(ImDrawVert));
            memcpy(index_dst + index_offset, indices.data(), indices.size() * sizeof(ImDrawIdx));
            g_rhi->RHIUnmapBuffer(staging_vertex_buffer);
            g_rhi->RHIUnmapBuffer(staging_index_buffer);
        });
        // memcpy(vertex_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        // memcpy(index_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

        // vertex_dst += cmd_list->VtxBuffer.Size;
        // index_dst += cmd_list->IdxBuffer.Size;

        vertex_offset += cmd_list->VtxBuffer.Size;
        index_offset += cmd_list->IdxBuffer.Size;
    }
    // g_rhi->RHIUnmapBuffer(render_buffers->staging_vertex_buffer);
    // g_rhi->RHIUnmapBuffer(render_buffers->staging_index_buffer);

    EnqueueRenderTask([_next_frame_info_render_thread,
                       _ui_command_list,
                       viewport_data,
                       num_frames_in_flight] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

        GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % num_frames_in_flight];

        RHIBufferRef staging_vertex_buffer = render_buffers->staging_vertex_buffer;
        RHIBufferRef staging_index_buffer  = render_buffers->staging_index_buffer;
        RHIBufferRef vertex_buffer         = render_buffers->vertex_buffer;
        RHIBufferRef index_buffer          = render_buffers->index_buffer;

        RHICopyBufferInfo copy_info{};
        copy_info.regions.emplace_back(0,
                                       0,
                                       staging_vertex_buffer->GetByteSize());
        RHICopyBufferInfo copy_index_info{};
        copy_index_info.regions.emplace_back(0,
                                             0,
                                             staging_index_buffer->GetByteSize());

        RHIBarrierDependencyInfo dependency_info{};
        auto&                    buffer_barriers = dependency_info.buffer_barriers;
        buffer_barriers.resize(2);
        buffer_barriers[0]
            .SetBuffer(vertex_buffer)
            .SetOffset(0)
            .SetSize(Moer::MAX_INT64)
            .SetSrcAccessFlags(ERHIAccessFlags::VERTEX_ATTRIBUTE_READ)
            .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetSrcStage(PS_VERTEX_INPUT)
            .SetDstStage(PS_TRANSFER);

        buffer_barriers[1]
            .SetBuffer(index_buffer)
            .SetOffset(0)
            .SetSize(Moer::MAX_INT64)
            .SetSrcAccessFlags(ERHIAccessFlags::INDEX_READ)
            .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetSrcStage(PS_VERTEX_INPUT)
            .SetDstStage(PS_TRANSFER);

        _ui_command_list->SetPipelineBarrier(dependency_info);

        _ui_command_list->CopyBuffer(copy_info, staging_vertex_buffer, vertex_buffer);
        _ui_command_list->CopyBuffer(copy_index_info, staging_index_buffer, index_buffer);

        buffer_barriers[0]
            .SetBuffer(vertex_buffer)
            .SetOffset(0)
            .SetSize(Moer::MAX_INT64)
            .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetDstAccessFlags(ERHIAccessFlags::VERTEX_ATTRIBUTE_READ)
            .SetSrcStage(PS_TRANSFER)
            .SetDstStage(PS_VERTEX_INPUT);

        buffer_barriers[1]
            .SetBuffer(index_buffer)
            .SetOffset(0)
            .SetSize(Moer::MAX_INT64)
            .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetDstAccessFlags(ERHIAccessFlags::INDEX_READ)
            .SetSrcStage(PS_TRANSFER)
            .SetDstStage(PS_VERTEX_INPUT);

        _ui_command_list->SetPipelineBarrier(dependency_info);
    });
}
void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
    ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);
    // std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;
    // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);

    GuiBackendData* backend_data = GetBackendData();

    GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

    // GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % backend_data->num_frames_in_flight];
    //todo frame_index should not manage here

    ImVec2 clip_off   = draw_data->DisplayPos;      // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale;// (1,1) unless using retina display which are often (2,2)
    SetupRenderState(draw_data, _ui_command_list, viewport_data, _next_frame_info_render_thread);
    int32_t global_vertex_offset = 0,
            global_index_offset  = 0;

    // ImVec2 clip_off = draw_data->DisplayPos;
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (uint32_t cmd_index = 0; cmd_index < cmd_list->CmdBuffer.Size; ++cmd_index) {
            const ImDrawCmd* cmd = &cmd_list->CmdBuffer[cmd_index];
            if (cmd->UserCallback != nullptr) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    SetupRenderState(draw_data, _ui_command_list, viewport_data, _next_frame_info_render_thread);
                } else {
                    cmd->UserCallback(cmd_list, cmd);
                }
            } else {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min((cmd->ClipRect.x - clip_off.x) * clip_scale.x, (cmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((cmd->ClipRect.z - clip_off.x) * clip_scale.x, (cmd->ClipRect.w - clip_off.y) * clip_scale.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                Rect2D r = {
                    (int32_t)clip_min.x,
                    (int32_t)clip_min.y,
                    (uint32_t)(clip_max.x - clip_min.x),
                    uint32_t(clip_max.y - clip_min.y)};

                // 5. local: set scissor and viewport
                EnqueueRenderTask([_ui_command_list, r, _next_frame_info_render_thread] {
                    if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
                    _ui_command_list->SetScissor(r);
                    // _ui_command_list->SetViewPort(g_rhi->RHIGetMainViewport()->GetViewportExtent());
                });

                // _ui_command_list->SetScissor(r);
                // _ui_command_list->SetViewPort(g_rhi->RHIGetMainViewport()->GetViewportExtent());

                // 6. local: set texture
                RHISRV* texture_view = (RHISRV*)cmd->GetTexID();

                ImGuiShaderFrag::Parameters params;
                params.texture0 = texture_view;
                RHIBatchedShaderParameters batched_params;

                batched_params.SetParameters(backend_data->shader_module_frag, params);

                RHIGraphicsPipelineStateRef pipeline = backend_data->pipeline;

                uint32_t elem_count = cmd->ElemCount;
                uint32_t vtx_offset = cmd->VtxOffset + global_vertex_offset;
                uint32_t idx_offset = cmd->IdxOffset + global_index_offset;

                EnqueueRenderTask([_ui_command_list,
                                   batched_params,
                                   pipeline,
                                   elem_count,
                                   vtx_offset,
                                   idx_offset,
                                   _next_frame_info_render_thread] {
                    if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
                    g_rhi->RHISetBatchedShaderParameters(pipeline, batched_params);

                    _ui_command_list->DrawIndexedInstanced(elem_count, 1, idx_offset, vtx_offset, 0);
                });

                // g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

                // 7. local: draw indexed instanced
                // _ui_command_list->DrawIndexedInstanced(cmd->ElemCount, 1, cmd->IdxOffset + global_index_offset, cmd->VtxOffset + global_vertex_offset, 0);
            }
        }
        global_index_offset += cmd_list->IdxBuffer.Size;
        global_vertex_offset += cmd_list->VtxBuffer.Size;
    }

    EnqueueRenderTask([viewport_data, _next_frame_info_render_thread] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
        viewport_data->frame_index += 1;
    });
}

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiViewportData* _viewport_data, RHIViewportNextBackBufferInfo* _next_frame_info_render_thread) {
    GuiBackendData* backend_data = GetBackendData();

    uint32_t num_frames_in_flight = backend_data->num_frames_in_flight;
    // 1. bind pipeline
    RHIGraphicsPipelineStateRef pipeline = backend_data->pipeline;
    EnqueueRenderTask([commandList, pipeline, _next_frame_info_render_thread] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
        commandList->SetPipelineState(pipeline);
    });
    // commandList->SetPipelineState(backend_data->pipeline);

    // 2. global: push constants
    ImGuiShaderVert::Parameters vert_param;
    ImGuiShaderFrag::Parameters frag_param;
    frag_param.sampler0 = backend_data->font_sampler;
    std::memset(&vert_param.vertexBuffer, 0, sizeof(vert_param.vertexBuffer));
    {
        float l         = draw_data->DisplayPos.x;
        float r         = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float t         = draw_data->DisplayPos.y;
        float b         = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        float mvp[4][4] = {
            {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
            {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
            {0.0f, 0.0f, 0.5f, 0.0f},
            {(r + l) / (l - r), (t + b) / (b - t), 0.5f, 1.0f},
        };
        memcpy(&vert_param.vertexBuffer.mvp, mvp, sizeof(mvp));
    }
    RHIBatchedShaderParameters batched_params;
    batched_params.SetParameters(backend_data->shader_module_vert, vert_param);
    batched_params.SetParameters(backend_data->shader_module_frag, frag_param);
    // should internally allocate descriptor set for vulkan if not allocated
    EnqueueRenderTask([commandList, batched_params, pipeline, _next_frame_info_render_thread] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;
        g_rhi->RHISetBatchedShaderParameters(pipeline, batched_params, true);
    });
    // g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params, true);

    // 3. global: set viewport, MARK: does it work ?
    ViewPort view_port(0, 0, draw_data->DisplaySize.x * draw_data->FramebufferScale.x, draw_data->DisplaySize.y * draw_data->FramebufferScale.y, 0.f, 1.f);

    EnqueueRenderTask([commandList, view_port, _viewport_data, num_frames_in_flight, _next_frame_info_render_thread] {
        if (_next_frame_info_render_thread->backbuffer_index == UINT32_MAX) return;

        GuiFrameRenderBuffers* render_buffers = &_viewport_data->render_buffers[_viewport_data->frame_index % num_frames_in_flight];

        RHIBufferRef vertex_buffer = render_buffers->vertex_buffer;
        RHIBufferRef index_buffer  = render_buffers->index_buffer;

        commandList->SetViewPort(view_port);
        // 4. global: bind vertex/index
        uint32_t offsets[] = {0};
        commandList->BindVertexBuffers(0, 1, &vertex_buffer, offsets);
        commandList->BindIndexBuffer(index_buffer, 0, EIndexElementType::IET_UINT16);
    });
}

//
void InvalidateDeviceObjects() {
    GuiBackendData* bd = GetBackendData();
    if (!bd)
        return;

    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->SetTexID(0);// We copied bd->pFontTextureView to io.Fonts->TexID so let's clear that as well.
}
bool CreateDeviceObjects() {
    GuiBackendData* backend_data = GetBackendData();
    if (!backend_data)
        return false;
    if (backend_data->pipeline)
        InvalidateDeviceObjects();

    RHISamplerCreateInfo sampler_init(ESamplerFilter::SF_CUBIC,
                                      TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    backend_data->font_sampler = g_rhi->RHICreateSampler(sampler_init.SetCompareOp(SCF_ALWAYS));

    auto&        shader_resource_manager = ShaderResourceManager::GetInstance();
    RHIShaderRef gui_vert                = shader_resource_manager.GetShader<ImGuiShaderVert>();
    RHIShaderRef gui_frag                = shader_resource_manager.GetShader<ImGuiShaderFrag>();

    RHIVertexInputInfo vertex_input_info(
        VertexElement(0, IM_OFFSETOF(ImDrawVert, pos), PF_R32G32_SFLOAT, 0, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX),
        VertexElement(0, IM_OFFSETOF(ImDrawVert, uv), PF_R32G32_SFLOAT, 1, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX),
        VertexElement(0, IM_OFFSETOF(ImDrawVert, col), PF_R8G8B8A8_UNORM, 2, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX));

    RHIGraphicsPSOCreateInfo pso_create_info =
        std::move(RHIGraphicsPSOCreateInfo::Create()
                      .SetRasterizerInfo(RHIRasterizeInfo::Preset())
                      .SetMultisampleInfo(RHIMultisampleStateInfo::Preset())
                      .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset())
                      .SetShaderStage(std::move(RHIGraphicsShaderInputInfo::Create()
                                                    .SetVertexWorkFlow(
                                                        std::move(vertex_input_info),
                                                        gui_vert,
                                                        gui_frag)))
                      .SetColorAttachmentInfo(
                          {RHIColorAttachmentInfo::Preset(g_rhi->RHIGetMainViewport()->GetViewportInfo().backbuffer_format)
                               .SetBlendStateInfo(RHIBlendAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>())})
                      .Finalize());

    backend_data->pipeline = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));
    // setup backend data
    backend_data->shader_module_vert = gui_vert;
    backend_data->shader_module_frag = gui_frag;

    //font texture and srv
    CreateFontsTexture();

    return true;
};

void CreateFontsTexture() {
    ImGuiIO&        io           = ImGui::GetIO();
    GuiBackendData* backend_data = GetBackendData();

    uint8_t* pixels;

    int width, height;
    //MARK... this is freaking slow, it's build first called, we need a default data for it, and async load other fonts
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    //upload texture
    {
        const uint32_t alignment    = 256;
        RHITextureRef  font_texture = nullptr;

        font_texture = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create("GuiFontTexture2D", ETextureDimension::TEX_2D)
                                                   .SetNumSamples(1)
                                                   .SetExtent({width, height})
                                                   .SetNumMips(1)
                                                   .SetArraySize(1)
                                                   .SetFormat(PF_R8G8B8A8_UNORM)
                                                   .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::SRGB | ETextureUsageFlags::TRANSFER_DST)
                                                   .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));

        uint32_t upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
        uint32_t upload_size  = height * upload_pitch;

        RHIBufferRef staging_buffer = g_rhi->RHICreateBuffer<std::byte>(
            upload_size, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE);

        assert(font_texture.Get() && staging_buffer.Get());

        void* mapped = g_rhi->RHIMapBuffer(staging_buffer, 0, upload_size);

        memcpy(mapped, pixels, upload_size);

        g_rhi->RHIUnmapBuffer(staging_buffer);

        RHISubresourceRange range{ETextureAspectFlags::COLOR,
                                  0,
                                  1,
                                  0,
                                  1,
                                  0,
                                  1};

        RHITextureBarrierInfo tex_barriers[2];

        tex_barriers[0].src_layout = TEXTURE_LAYOUT_UNDEFINED;
        tex_barriers[0].dst_layout = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[0].src_access = ERHIAccessFlags::UNDEFINED;
        tex_barriers[0].dst_access = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[0].dst_stage  = PS_TRANSFER;

        tex_barriers[0].p_texture          = font_texture;
        tex_barriers[0].sub_resource_range = range;

        tex_barriers[1].src_layout         = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[1].dst_layout         = TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex_barriers[1].src_access         = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[1].dst_access         = ERHIAccessFlags::SHADER_READ;
        tex_barriers[1].src_stage          = PS_TRANSFER;
        tex_barriers[1].dst_stage          = PS_FRAGMENT_SHADER;
        tex_barriers[1].p_texture          = font_texture;
        tex_barriers[1].sub_resource_range = range;

        RHIGraphicsCommandList* command_list = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());

        RHIBarrierDependencyInfo font_create_barriers{};
        font_create_barriers.texture_barriers.resize(1);
        font_create_barriers.texture_barriers[0] = tex_barriers[0];

        command_list->BeginRecording();
        command_list->SetPipelineBarrier(font_create_barriers);

        RHISubresourceSlice        resource_slice(ETextureAspectFlags::COLOR, 0, 0, 1, 0, 1);
        RHICopyBufferToTextureInfo copy_info(
            ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST,
            {0, 0, 0},
            {(uint32_t)width, (uint32_t)height, 1},
            resource_slice,
            0);

        // 3. MARK: pRegion[0] is trying to copy 518144 bytes plus 0 offset to/from the VkBuffer (VkBuffer 0xcb1c7c000000001b[]) which exceeds the VkBuffer total size of 131072 bytes.
        command_list->CopyBufferToTexture(copy_info, staging_buffer, font_texture);

        RHIBarrierDependencyInfo font_copy_barriers{};
        font_copy_barriers.texture_barriers.resize(1);
        font_copy_barriers.texture_barriers[0] = tex_barriers[1];

        command_list->SetPipelineBarrier(font_copy_barriers);

        RHIBatchedShaderParameters  batched_params;
        ImGuiShaderFrag::Parameters params;
        params.sampler0 = backend_data->font_sampler;
        params.texture0 = backend_data->font_view;

        batched_params.SetParameters(backend_data->shader_module_frag, params);
        g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

        command_list->EndRecording();

        RHICommandQueue* queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

        RHIFenceCreateInfo fence_info{EFenceUsageFlags::TIMELINE};
        RHIFenceRef        fence = g_rhi->RHICreateFence(fence_info);

        RHISubmitInfo submit_info;

        uint64_t wait_value = 1;
        submit_info.Signal(fence, wait_value);
        queue->SubmitCommands(1, command_list, &submit_info);

        fence->Wait(wait_value);

        auto srv_info = RHIViewInfo::CreateTextureSRVInfo()
                            .SetFormat(PF_R8G8B8A8_UNORM)
                            .SetDimension(ETextureDimension::TEX_2D)
                            .SetMipRange(0, 1)
                            .SetArrayRange(0, 1);

        backend_data->font_view    = g_rhi->RHICreateTextureSRV(font_texture);
        backend_data->font_texture = font_texture;
    }
    io.Fonts->SetTexID((ImTextureID)backend_data->font_view);
}

void GuiCreateWindow(ImGuiViewport* viewport);
void GuiDestroyWindow(ImGuiViewport* viewport);
void GuiSetWindowSize(ImGuiViewport* viewport, ImVec2 size);
void GuiRenderWindow(ImGuiViewport* viewport, void*);

void GuiInitPlatformInterface() {
    ImGuiPlatformIO& platform_io       = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow  = GuiCreateWindow;
    platform_io.Renderer_DestroyWindow = GuiDestroyWindow;
    platform_io.Renderer_SetWindowSize = GuiSetWindowSize;
    platform_io.Renderer_RenderWindow  = GuiRenderWindow;
    platform_io.Renderer_SwapBuffers   = GuiSwapbuffer;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers) {
    _render_buffers->index_buffer          = nullptr;
    _render_buffers->vertex_buffer         = nullptr;
    _render_buffers->staging_index_buffer  = nullptr;
    _render_buffers->staging_vertex_buffer = nullptr;
}

void GuiCreateWindow(ImGuiViewport* viewport) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = MoerNew(GuiViewportData)(backend_data->num_frames_in_flight);

    viewport->RendererUserData = viewport_data;

    viewport_data->command_queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

    viewport_data->comand_list = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());

    RHIFenceCreateInfo present_fence_info{EFenceUsageFlags::PRESENT};
    viewport_data->present_fence = g_rhi->RHICreateFence(present_fence_info);
    // viewport_data->present_fence = backend_data->
    RHIViewportInitializer viewport_info;

    Moer::WindowHandle handle{
        (Moer::WindowType*)(viewport->PlatformHandle ?
                                viewport->PlatformHandle :
                                viewport->PlatformHandleRaw)};
    viewport_info.window_handle = &handle;

    //TODO: should be controlled by UI
    viewport_info.b_vsync = false;
    // viewport_info.window_handle = viewport->PlatformHandleRaw;
    viewport_data->frame_index = 0;

    viewport_data->viewport = g_rhi->RHICreateViewport(viewport_info);
}

void GuiDestroyWindow(ImGuiViewport* viewport) {
    GuiBackendData* backend_data = GetBackendData();

    if (GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData) {
        if (viewport_data && viewport_data->command_queue && viewport_data->present_fence) {
            RHIViewportRef viewport = viewport_data->viewport;

            RHICommandQueue*        command_queue = viewport_data->command_queue;
            RHIGraphicsCommandList* command_list  = viewport_data->comand_list;
            RHIFenceRef             present_fence = viewport_data->present_fence;
            EnqueueRenderTask([viewport, command_queue, command_list, present_fence] {
                viewport->WaitForQueueComplete(command_queue, present_fence);
                MoerDelete(command_list);
                MoerDelete(command_queue);
            });
            Moer::RenderThreadFence fence;
            fence.BeginFence();
            fence.Wait();

            // MoerDelete(viewport_data->comand_list);
            viewport_data->comand_list = nullptr;
            // MoerDelete(viewport_data->command_queue);
            viewport_data->command_queue = nullptr;
            // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
            for (uint32_t i = 0; i < backend_data->num_frames_in_flight; i++)
                DestroyRenderBuffers(&viewport_data->render_buffers[i]);
            MoerDelete(viewport_data);
        }
    }
    viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

    auto m_viewport = viewport_data->viewport;

    EnqueueRenderTask([m_viewport, size] {
        Extent2D viewport_extent = {(uint32_t)m_viewport->GetViewportExtent().width, (uint32_t)m_viewport->GetViewportExtent().height};

        if (size.x == viewport_extent.width && size.y == viewport_extent.height) return;
        g_rhi->RHIResizeViewport(m_viewport, Extent2D(size.x, size.y), false);
    });
    // g_rhi->RHIResizeViewport(m_viewport, Extent2D(size.x, size.y), false);
}
void GuiRenderWindow(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;
    RHIViewport*     rhi_viewport  = viewport_data->viewport;
    EnqueueRenderTask([rhi_viewport, viewport_data] {
        viewport_data->next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(rhi_viewport);
        if (viewport_data->next_frame_info.backbuffer_index == UINT32_MAX) return;

        RHIUAV* present_view = g_rhi->RHIGetViewportBackBufferUAV(rhi_viewport, viewport_data->next_frame_info.backbuffer_index);

        //transfer present texture layout to color attachment layout
        RHIBarrierDependencyInfo dependency_info;
        dependency_info.texture_barriers.resize(1);
        auto& texture_barriers = dependency_info.texture_barriers;

        texture_barriers[0]
            .SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT)
            .SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC)
            .SetTexture(present_view->GetTexture())
            .SetSrcStage(PS_BOTTOM_OF_PIPE)
            .SetDstStage(PS_COLOR_ATTACHMENT_OUTPUT)
            .SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED)
            .SetDstAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

        //transfer present texture layout to present src

        viewport_data->present_fence->Wait(viewport_data->frame_index);
        viewport_data->comand_list->Reset();

        viewport_data->comand_list->BeginRecording();

        viewport_data->comand_list->SetPipelineBarrier(dependency_info);
    });
    // RHIViewportNextBackBufferInfo info = g_rhi->RHIGetNextFrameViewportBufferInfo(rhi_viewport);

    GUIUploadData(viewport->DrawData, viewport_data->comand_list, &viewport_data->next_frame_info);

    EnqueueRenderTask([viewport_data] {
        if (viewport_data->next_frame_info.backbuffer_index == UINT32_MAX) return;

        RHIUAV* present_view = g_rhi->RHIGetViewportBackBufferUAV(viewport_data->viewport, viewport_data->next_frame_info.backbuffer_index);

        RHIRenderPassInfo pass_info;
        pass_info.color_attachments[0].color_attachment_action                = AC_CLEAR_STORE;
        pass_info.color_attachments[0].color_attachment_view.texture_view     = present_view;
        pass_info.color_attachments[0].color_attachment_view.required_layout  = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;
        pass_info.color_attachments[0].color_attachment_view.clear_attachment = RHIClearAttachment();

        auto viewport_extent                = viewport_data->viewport->GetViewportExtent();
        pass_info.render_area.offset.x      = 0;
        pass_info.render_area.offset.y      = 0;
        pass_info.render_area.extent.width  = viewport_extent.width;
        pass_info.render_area.extent.height = viewport_extent.height;
        viewport_data->comand_list->BeginRenderPass(pass_info, "Imgui Window");
    });

    GUIRender(viewport->DrawData, viewport_data->comand_list, &viewport_data->next_frame_info);
    EnqueueRenderTask([viewport_data] {
        if (viewport_data->next_frame_info.backbuffer_index == UINT32_MAX) return;
        RHIUAV* present_view = g_rhi->RHIGetViewportBackBufferUAV(viewport_data->viewport, viewport_data->next_frame_info.backbuffer_index);

        viewport_data->comand_list->EndRenderPass();

        RHIBarrierDependencyInfo texture_dependency_info;
        texture_dependency_info.texture_barriers.resize(1);
        auto& texture_barriers_present = texture_dependency_info.texture_barriers;
        texture_barriers_present[0]
            .SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC)
            .SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT)
            .SetTexture(present_view->GetTexture())
            .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
            .SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT);
        viewport_data->comand_list->SetPipelineBarrier(texture_dependency_info);

        viewport_data->comand_list->EndRecording();

        RHISubmitInfo submit_info{};

        //wait for last frame recording
        submit_info.Wait(viewport_data->present_fence, viewport_data->frame_index - 1);
        //wait for back_buffer ready
        submit_info.Wait(viewport_data->next_frame_info.backbuffer_ready_fence, 0);
        //signal this frame present fence
        submit_info.Signal(viewport_data->present_fence, viewport_data->frame_index);

        viewport_data->command_queue->SubmitCommands(1, viewport_data->comand_list, &submit_info);
    });
}

void GuiSwapbuffer(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

    //present wait for this frame rendering end fence
    // viewport_data->viewport->Present(viewport_data->present_fence);
    EnqueueRenderTask([viewport_data] {
        if (viewport_data->next_frame_info.backbuffer_index == UINT32_MAX) return;
        // viewport_data->viewport->Present(viewport_data->present_fence);
        g_rhi->RHIPresentViewport(viewport_data->viewport, viewport_data->present_fence);
    });
    // g_rhi->RHIPresentViewport(viewport_data->viewport, viewport_data->present_fence);
}