#include "ImGUIRenderer.h"
#include "IconsFontAwesome6.h"
#include "RenderThread.h"
#include "config/ConfigManager.h"
#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"

#include "math/Math.h"

#include "taskgraph/GraphTask.h"
#include "taskgraph/ThreadManager.h"
#include "window/WindowContext.h"

#include <atomic>
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
};
struct GuiBackendData {
    size_t buffer_memory_alignment;

    RHIGraphicsPipelineStateRef pipeline;
    RHIVertexShaderRef          shader_module_vert;
    RHIFragmentShaderRef        shader_module_frag;

    // Font data
    RHISamplerRef            font_sampler;
    RHITextureRef            font_texture;
    RHIShaderResourceViewRef font_view;
    RHIBufferRef             upload_buffer;

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
    DEFINE_SHADER_TYPE(ImGuiShaderVert, Global, RHI_API, ...)
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
    DEFINE_SHADER_TYPE(ImGuiShaderFrag, Global, RHI_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)

    DEFINE_SHADER_PARAM_SAMPLER(SamplerState, sampler0)
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
};
void GuiInitPlatformInterface();
bool CreateDeviceObjects();
void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);
void InvalidateDeviceObjects();

void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list);

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

    GuiFrameRenderBuffers* render_buffers;// Used by all viewports

    uint64_t        frame_index;
    uint32_t        viewport_index;
    static uint32_t viewport_count;

    GuiViewportData(uint32_t _frame_in_flight) {
        memset((void*)this, 0, sizeof(*this));
        render_buffers = new GuiFrameRenderBuffers[_frame_in_flight];
        for (uint32_t i = 0; i < _frame_in_flight; ++i) {
            render_buffers[i].vertex_buffer = nullptr;
            render_buffers[i].index_buffer  = nullptr;
        }
        viewport_index = viewport_count;
        viewport_count++;
    }
    ~GuiViewportData() {
        delete[] render_buffers;
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

    GuiBackendData* render_backend_data       = IM_NEW(GuiBackendData)();
    render_backend_data->num_frames_in_flight = max_frame_in_flight;

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiViewport*   main_viewport  = ImGui::GetMainViewport();
    GuiViewportData* viewport_data  = IM_NEW(GuiViewportData)(max_frame_in_flight);
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

        auto next_frame_info = g_rhi->RHIGetNextFrameViewportBufferInfo(main_viewport);

        if (next_frame_info.backbuffer_index == UINT32_MAX) return;

        RHIUnorderedAccessView*  present_view = g_rhi->RHIGetViewportBackBufferUAV(main_viewport, next_frame_info.backbuffer_index);
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

        ui_command_list->Open();
        ui_command_list->SetPipelineBarrier(dependency_info);

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

        auto* draw_data = ImGui::GetDrawData();

        GUIRender(main_draw_data, ui_command_list);

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

        ui_command_list->Close();

        {
            // Update and Render additional Platform Windows
            // May Change in the future
            auto& io = ImGui::GetIO();
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault(nullptr, nullptr);
            }
        }

        RHISubmitInfo submit_info{};

        //wait for last frame recording(don't need if wait before reseting command list)
        submit_info.Wait(present_fence, timeline_index);
        //wait for back_buffer ready
        submit_info.Wait(next_frame_info.backbuffer_ready_fence, 0);
        //signal this frame present fence
        submit_info.Signal(present_fence, ++timeline_index);

        command_queue->SubmitCommands(1, ui_command_list, &submit_info);

        ImGuiIO& io = ImGui::GetIO();

        ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

        if (io.BackendFlags & ImGuiBackendFlags_RendererHasViewports) {
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiRenderWindow(platform_io.Viewports[i], nullptr);
            for (int i = 1; i < platform_io.Viewports.Size; i++)
                if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
                    GuiSwapbuffer(platform_io.Viewports[i], nullptr);
        }

        g_rhi->RHIPresentViewport(main_viewport, present_fence);
    }
}

void ImGUIRenderer::Impl::ShutDown() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    // Manually delete main viewport render resources in-case we haven't initialized for viewports
    auto* main_viewport = ImGui::GetDrawData()->OwnerViewport;
    if (GuiViewportData* viewport_data = (GuiViewportData*)main_viewport->RendererUserData) {
        // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
        for (uint32_t i = 0; i < bd->num_frames_in_flight; i++)
            DestroyRenderBuffers(&viewport_data->render_buffers[i]);
        IM_DELETE(viewport_data);
        main_viewport->RendererUserData = nullptr;
    }

    // Clean up windows and device objects
    ImGui::DestroyPlatformWindows();
    InvalidateDeviceObjects();

    io.BackendRendererName     = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);
    IM_DELETE(bd);
    command_queue->WaitForQueueComplete();
    MoerDelete(command_queue);
    MoerDelete(ui_command_list);
}

uint32_t GuiViewportData::viewport_count = 0;

GuiBackendData::~GuiBackendData() {
}

bool CreateDeviceObjects();
void CreateFontsTexture();

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiFrameRenderBuffers* render_buffers);

void GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list) {
    ImDrawData* draw_data = static_cast<ImDrawData*>(_draw_data);
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;
    Shader* frag_shader = ShaderResourceManager::GetShader<ImGuiShaderFrag>();
    Shader* vert_shader = ShaderResourceManager::GetShader<ImGuiShaderVert>();
    // RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);

    GuiBackendData* backend_data = GetBackendData();

    GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

    GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % backend_data->num_frames_in_flight];

    RHIBufferRef staging_vertex_buffer;
    RHIBufferRef staging_index_buffer;
    //todo frame_index should not manage here
    viewport_data->frame_index += 1;
    if (render_buffers->vertex_buffer == nullptr || render_buffers->vertex_buffer->GetSize() < draw_data->TotalVtxCount * sizeof(ImDrawVert)) {
        //delete the old one and create new
        if (render_buffers->vertex_buffer != nullptr) {}
        // render_buffers->vertex_buffer->DeRef();
        uint32_t new_size             = (draw_data->TotalVtxCount + 4096) * sizeof(ImDrawVert);
        render_buffers->vertex_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(
                new_size,
                sizeof(ImDrawVert),
                EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE));

        // staging_index_buffer = g_rhi->RHICreateBuffer(
        //     RHIBufferCreateInfo::Create(
        //         new_size,
        //         sizeof(ImDrawVert),
        //         EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));
    }
    if (render_buffers->index_buffer == nullptr || render_buffers->index_buffer->GetSize() < draw_data->TotalIdxCount * sizeof(ImDrawIdx)) {

        if (render_buffers->index_buffer != nullptr) {}
        uint32_t new_size            = (draw_data->TotalIdxCount + 8192) * sizeof(ImDrawIdx);
        render_buffers->index_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(
                new_size,
                sizeof(ImDrawIdx),
                EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE));

        // staging_index_buffer = g_rhi->RHICreateBuffer(
        //     RHIBufferCreateInfo::Create(
        //         new_size,
        //         sizeof(ImDrawIdx),
        //         EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));
    }

    ImDrawVert* vertex_dst = nullptr;
    ImDrawIdx*  index_dst  = nullptr;

    vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(render_buffers->vertex_buffer, 0, UINT64_MAX);
    index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(render_buffers->index_buffer, 0, UINT64_MAX);
    // vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(staging_vertex_buffer, 0, UINT64_MAX);
    // index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(staging_index_buffer, 0, UINT64_MAX);
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        memcpy(vertex_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(index_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

        vertex_dst += cmd_list->VtxBuffer.Size;
        index_dst += cmd_list->IdxBuffer.Size;
    }
    g_rhi->RHIUnmapBuffer(render_buffers->vertex_buffer);
    g_rhi->RHIUnmapBuffer(render_buffers->index_buffer);
    // g_rhi->RHIUnmapBuffer(staging_vertex_buffer);
    // g_rhi->RHIUnmapBuffer(staging_index_buffer);

    RHICopyBufferInfo copy_info{};

    // _ui_command_list->CopyBuffer(render_buffers->vertex_buffer, staging_vertex_buffer, 0, 0, staging_vertex_buffer->GetSize());

    ImVec2 clip_off   = draw_data->DisplayPos;      // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale;// (1,1) unless using retina display which are often (2,2)
    SetupRenderState(draw_data, _ui_command_list, render_buffers);
    int32_t global_vertex_offset = 0,
            global_index_offset  = 0;

    // ImVec2 clip_off = draw_data->DisplayPos;
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (uint32_t cmd_index = 0; cmd_index < cmd_list->CmdBuffer.Size; ++cmd_index) {
            const ImDrawCmd* cmd = &cmd_list->CmdBuffer[cmd_index];
            if (cmd->UserCallback != nullptr) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    SetupRenderState(draw_data, _ui_command_list, render_buffers);
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
                _ui_command_list->SetScissor(r);
                // _ui_command_list->SetViewPort(g_rhi->RHIGetMainViewport()->GetViewportExtent());

                // 6. local: set texture
                RHIShaderResourceView* texture_view = (RHIShaderResourceView*)cmd->GetTexID();

                ImGuiShaderFrag::Parameters params;
                params.texture0 = texture_view;
                RHIBatchedShaderParameters batched_params;
                batched_params.SetParameters(frag_shader, params);
                g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

                // 7. local: draw indexed instanced
                _ui_command_list->DrawIndexedInstanced(cmd->ElemCount, 1, cmd->IdxOffset + global_index_offset, cmd->VtxOffset + global_vertex_offset, 0);
            }
        }
        global_index_offset += cmd_list->IdxBuffer.Size;
        global_vertex_offset += cmd_list->VtxBuffer.Size;
    }
}

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiFrameRenderBuffers* render_buffers) {
    GuiBackendData* backend_data = GetBackendData();
    // 1. bind pipeline
    commandList->SetPipelineState(backend_data->pipeline);

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
    g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params, true);

    // 3. global: set viewport, MARK: does it work ?
    ViewPort view_port(0, 0, draw_data->DisplaySize.x * draw_data->FramebufferScale.x, draw_data->DisplaySize.y * draw_data->FramebufferScale.y, 0.f, 1.f);
    commandList->SetViewPort(view_port);

    // 4. global: bind vertex/index
    uint32_t offsets[] = {0};
    commandList->BindVertexBuffers(0, 1, &render_buffers->vertex_buffer, offsets);
    commandList->BindIndexBuffer(render_buffers->index_buffer.Get(), 0, EIndexElementType::IET_UINT16);
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

    RHISamplerInitializer sampler_init(ESamplerFilter::SF_CUBIC, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sampler_init.compare_op    = CO_ALWAYS;
    RHISamplerRef sampler      = g_rhi->RHICreateSampler(sampler_init);
    backend_data->font_sampler = sampler;

    RHIBlendStateInitializer blend_state_info;
    blend_state_info.attachments[0].color_blend_op         = BO_ADD;
    blend_state_info.attachments[0].color_src_blend_factor = BF_SRC_ALPHA;
    blend_state_info.attachments[0].color_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].alpha_blend_op         = BO_ADD;
    blend_state_info.attachments[0].alpha_src_blend_factor = BF_ONE;
    blend_state_info.attachments[0].alpha_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].color_write_mask       = CW_RGBA;

    auto blend_state = g_rhi->RHICreateBlendState(blend_state_info);

    RHIRasterizationStateInitializer rast_init{};
    rast_init.fill_mode              = FM_FILL;
    rast_init.cull_mode              = RCM_NONE;
    rast_init.depth_bias             = 0;
    rast_init.depth_bias_clamp       = 0.f;
    rast_init.depth_bias_slop_factor = 0.f;
    rast_init.b_depth_clamp_enable   = true;
    rast_init.b_enable_msaa          = false;
    rast_init.b_depth_bias           = false;

    RHIMultisampleStateInitializer msaa_init{};
    msaa_init.sample_count = 1;

    RHIDepthStencilStateInitializer depth_stencil_init{};
    depth_stencil_init.depth_test_op                    = CO_ALWAYS;
    depth_stencil_init.b_enable_depth_write             = false;
    depth_stencil_init.b_enable_front_face_stencil      = false;
    depth_stencil_init.b_enable_back_face_stencil       = false;
    depth_stencil_init.front_face_depth_fail_stencil_op = SO_KEEP;
    depth_stencil_init.front_face_pass_stencil_op       = SO_KEEP;
    depth_stencil_init.front_face_stencil_test          = CO_ALWAYS;

    // VertexElement
    VertexInputStateInitializerList input_intializer{};
    input_intializer[0] = VertexElement(0, IM_OFFSETOF(ImDrawVert, pos), PF_R32G32_SFLOAT, 0, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[1] = VertexElement(0, IM_OFFSETOF(ImDrawVert, uv), PF_R32G32_SFLOAT, 1, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[2] = VertexElement(0, IM_OFFSETOF(ImDrawVert, col), PF_R8G8B8A8_UNORM, 2, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);

    RHIVertexShaderRef     gui_vert    = g_rhi->RHICreateVertexShader(ShaderResourceManager::GetShader<ImGuiShaderVert>());
    RHIFragmentShaderRef   gui_frag    = g_rhi->RHICreateFragmentShader(ShaderResourceManager::GetShader<ImGuiShaderFrag>());
    RHIVertexInputStateRef input_state = g_rhi->RHICreateVertexInputState(input_intializer);

    RHIGraphicsPipelineStateInitializer pso_init(
        blend_state,
        g_rhi->RHICreateRasterizationState(rast_init),
        g_rhi->RHICreateMultiSampleState(msaa_init),
        g_rhi->RHICreateDepthStencilState(depth_stencil_init),
        EPrimitiveTopology::TRIANGLE_LIST,
        1,
        {g_rhi->RHIGetMainViewport()->GetViewportInfo().backbuffer_format},
        {ETextureUsageFlags::COLOR_ATTACHMENT},
        PF_UNDEFINED,
        ETextureUsageFlags::UNDEFINED,
        {SubpassSettings::Type::NONE, 0},
        false,
        1,
        false,
        VSR_1_1x1);
    pso_init.shader_stage.p_vertex_shader      = gui_vert;
    pso_init.shader_stage.p_fragment_shader    = gui_frag;
    pso_init.shader_stage.p_vertex_input_state = input_state;

    auto c = pso_init.CalcValidColorAttachmentCount();

    backend_data->pipeline = g_rhi->RHICreateGraphicsPipelineState(pso_init);
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

        font_texture = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create("FontTexture2D", ETextureDimension::TEX_2D)
                                                   .SetNumSamples(1)
                                                   .SetExtent({width, height})
                                                   .SetNumMips(1)
                                                   .SetArraySize(1)
                                                   .SetFormat(PF_R8G8B8A8_UNORM)
                                                   .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::SRGB | ETextureUsageFlags::TRANSFER_DST)
                                                   .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));

        uint32_t upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
        uint32_t upload_size  = height * upload_pitch;

        RHIBufferRef staging_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(upload_size, 0, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));

        assert(font_texture.Get() && staging_buffer.Get());

        void* mapped = g_rhi->RHIMapBuffer(staging_buffer, 0, upload_size);
        // for (int32_t y = 0; y < height; y++) {
        //     memcpy((void*)((uint8_t*)mapped + y * upload_pitch), pixels + y * width * 4, width * 4);
        // }
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

        command_list->Open();
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

        command_list->Close();

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

        backend_data->font_view    = g_rhi->RHICreateShaderResourceView(font_texture, srv_info);
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
    _render_buffers->index_buffer  = nullptr;
    _render_buffers->vertex_buffer = nullptr;
}

void GuiCreateWindow(ImGuiViewport* viewport) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = IM_NEW(GuiViewportData)(backend_data->num_frames_in_flight);

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
            viewport_data->viewport->WaitForQueueComplete(viewport_data->command_queue, viewport_data->present_fence);

            MoerDelete(viewport_data->comand_list);
            viewport_data->comand_list = nullptr;
            MoerDelete(viewport_data->command_queue);
            viewport_data->command_queue = nullptr;
            // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
            for (uint32_t i = 0; i < backend_data->num_frames_in_flight; i++)
                DestroyRenderBuffers(&viewport_data->render_buffers[i]);
            IM_DELETE(viewport_data);
        }
    }
    viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

    auto m_viewport = viewport_data->viewport;

    g_rhi->RHIResizeViewport(m_viewport, Extent2D(size.x, size.y), false);
}
void GuiRenderWindow(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;
    RHIViewport*     rhi_viewport  = viewport_data->viewport;

    RHIViewportNextBackBufferInfo info = g_rhi->RHIGetNextFrameViewportBufferInfo(rhi_viewport);

    if (info.backbuffer_index == UINT32_MAX) return;

    RHIUnorderedAccessView* present_view = g_rhi->RHIGetViewportBackBufferUAV(rhi_viewport, info.backbuffer_index);

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
    RHIBarrierDependencyInfo texture_dependency_info;
    texture_dependency_info.texture_barriers.resize(1);
    auto& texture_barriers_present = texture_dependency_info.texture_barriers;
    texture_barriers_present[0]
        .SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC)
        .SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT)
        .SetTexture(present_view->GetTexture())
        .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
        .SetSrcStage(PS_COLOR_ATTACHMENT_OUTPUT);

    viewport_data->present_fence->Wait(viewport_data->frame_index);
    viewport_data->comand_list->Reset();

    viewport_data->comand_list->Open();

    viewport_data->comand_list->SetPipelineBarrier(dependency_info);

    RHIRenderPassInfo pass_info;
    pass_info.color_attachments[0].color_attachment_action                            = AC_CLEAR_STORE;
    pass_info.color_attachments[0].color_attachment_view.texture_view                 = present_view;
    pass_info.color_attachments[0].color_attachment_view.required_layout              = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;
    pass_info.color_attachments[0].color_attachment_view.clear_attachment.value.color = {0.0f, 0.0f, 0.0f, 1.0f};

    pass_info.render_area.offset.x      = 0;
    pass_info.render_area.offset.y      = 0;
    pass_info.render_area.extent.width  = viewport->Size.x;
    pass_info.render_area.extent.height = viewport->Size.y;

    viewport_data->comand_list->BeginRenderPass(pass_info, "Imgui Window");

    GUIRender(viewport->DrawData, viewport_data->comand_list);

    viewport_data->comand_list->EndRenderPass();

    viewport_data->comand_list->SetPipelineBarrier(texture_dependency_info);

    viewport_data->comand_list->Close();

    RHISubmitInfo submit_info{};

    //wait for last frame recording
    submit_info.Wait(viewport_data->present_fence, viewport_data->frame_index - 1);
    //wait for back_buffer ready
    submit_info.Wait(info.backbuffer_ready_fence, 0);
    //signal this frame present fence
    submit_info.Signal(viewport_data->present_fence, viewport_data->frame_index);

    viewport_data->command_queue->SubmitCommands(1, viewport_data->comand_list, &submit_info);
}

void GuiSwapbuffer(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;
    //present wait for this frame rendering end fence
    // viewport_data->viewport->Present(viewport_data->present_fence);
    g_rhi->RHIPresentViewport(viewport_data->viewport, viewport_data->present_fence);
}
