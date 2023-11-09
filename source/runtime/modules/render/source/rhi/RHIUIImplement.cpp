
#include "PixelFormat.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "RHIUIImplement.h"
#include "rhi/RHICommandList.h"
#include "rhi/RHICommandQueue.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"

#include <array>
#include <cstring>
#include <imgui.h>
#include <stdint.h>
#include <vadefs.h>
#include <vcruntime_string.h>

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

void                   GuiInitPlatformInterface();
void                   GuiRenderWindow(ImGuiViewport* viewport, void*);
inline GuiBackendData* GetBackendData() {
    return ImGui::GetCurrentContext() ? (GuiBackendData*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers);

bool CreateDeviceObjects();
void CreateFontsTexture();
void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiFrameRenderBuffers* render_buffers);
void InvalidateDeviceObjects();

bool RHI::GUIInit(uint32_t _num_frames_in_flight) {
    ImGuiIO& io = ImGui::GetIO();
    assert(io.BackendRendererUserData == nullptr && "GUI backend already initialized.");

    GuiBackendData* render_backend_data       = IM_NEW(GuiBackendData)();
    render_backend_data->num_frames_in_flight = 3;

    io.BackendRendererUserData = render_backend_data;
    io.BackendRendererName     = "Moer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    ImGuiViewport*   main_viewport  = ImGui::GetMainViewport();
    GuiViewportData* viewport_data  = IM_NEW(GuiViewportData)();
    main_viewport->RendererUserData = viewport_data;

    viewport_data->render_buffers = new GuiFrameRenderBuffers[_num_frames_in_flight];
    for (uint32_t i = 0; i < _num_frames_in_flight; i++) {
        GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[i];
        render_buffers->vertex_buffer         = nullptr;
        render_buffers->index_buffer          = nullptr;
    }

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        GuiInitPlatformInterface();

    return true;
}
void RHI::GUIShutDown() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();

    // Manually delete main viewport render resources in-case we haven't initialized for viewports
    ImGuiViewport* main_viewport = ImGui::GetMainViewport();
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
}
void RHI::GUINewFrame() {
    GuiBackendData* bd = GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call GuiInit(uint32_t)?");

    if (!bd->pipeline)
        CreateDeviceObjects();
}
void RHI::GUIRender(void* _draw_data, RHIGraphicsCommandList* _ui_command_list) {

    ImDrawData*          draw_data       = static_cast<ImDrawData*>(_draw_data);
    Shader*              frag_shader     = ShaderResourceManager::GetShader<ImGuiShaderFrag>();
    RHIFragmentShaderRef frag_rhi_shader = g_rhi->RHICreateFragmentShader(frag_shader);

    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)draw_data->OwnerViewport->RendererUserData;

    //todo frame_index should not manage here
    viewport_data->frame_index += 1;
    GuiFrameRenderBuffers* render_buffers = &viewport_data->render_buffers[viewport_data->frame_index % backend_data->num_frames_in_flight];

    if (render_buffers->vertex_buffer == nullptr || render_buffers->vertex_buffer->GetSize() < draw_data->TotalVtxCount * sizeof(ImDrawVert)) {
        //delete the old one and create new
        if (render_buffers->vertex_buffer != nullptr)
            render_buffers->vertex_buffer->DeRef();
        uint32_t new_size             = (draw_data->TotalVtxCount + 4096) * sizeof(ImDrawVert);
        render_buffers->vertex_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(
                new_size,
                sizeof(ImDrawVert),
                EBufferUsageFlags::VERTEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE));
    }
    if (render_buffers->index_buffer == nullptr || render_buffers->index_buffer->GetSize() < draw_data->TotalIdxCount * sizeof(ImDrawIdx)) {

        if (render_buffers->index_buffer != nullptr)
            render_buffers->index_buffer->DeRef();
        uint32_t new_size            = (draw_data->TotalIdxCount + 8192) * sizeof(ImDrawIdx);
        render_buffers->index_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(
                new_size,
                sizeof(ImDrawIdx),
                EBufferUsageFlags::INDEX_BUFFER | EBufferUsageFlags::CPU_VISIBLE));
    }

    ImDrawVert* vertex_dst = nullptr;
    ImDrawIdx*  index_dst  = nullptr;

    vertex_dst = (ImDrawVert*)g_rhi->RHIMapBuffer(render_buffers->vertex_buffer, 0, UINT64_MAX);
    index_dst  = (ImDrawIdx*)g_rhi->RHIMapBuffer(render_buffers->index_buffer, 0, UINT64_MAX);

    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        memcpy(vertex_dst, cmd_list->VtxBuffer.Data, cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(index_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));

        vertex_dst += cmd_list->VtxBuffer.Size;
        index_dst += cmd_list->IdxBuffer.Size;
    }
    g_rhi->RHIUnmapBuffer(render_buffers->vertex_buffer);
    g_rhi->RHIUnmapBuffer(render_buffers->index_buffer);

    SetupRenderState(draw_data, _ui_command_list, render_buffers);
    int32_t global_vertex_offset = 0,
            global_index_offset  = 0;

    ImVec2 clip_off = draw_data->DisplayPos;
    for (int32_t n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (int32_t cmd_index; cmd_list->CmdBuffer.Size; cmd_index++) {
            const ImDrawCmd* cmd = &cmd_list->CmdBuffer[cmd_index];
            if (cmd->UserCallback != nullptr) {
                if (cmd->UserCallback == ImDrawCallback_ResetRenderState) {
                    SetupRenderState(draw_data, _ui_command_list, render_buffers);
                } else {
                    cmd->UserCallback(cmd_list, cmd);
                }
            } else {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min(cmd->ClipRect.x - clip_off.x, cmd->ClipRect.y - clip_off.y);
                ImVec2 clip_max(cmd->ClipRect.z - clip_off.x, cmd->ClipRect.w - clip_off.y);
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                Rect2D r = {(int32_t)clip_min.x, (int32_t)clip_min.y, (uint32_t)(clip_max.x - clip_min.x), uint32_t(clip_max.y - clip_min.y)};

                RHIShaderResourceView* texture_view = (RHIShaderResourceView*)cmd->GetTexID();

                ImGuiShaderFrag::Parameters params;
                params.texture0 = texture_view;

                RHIBatchedShaderParameters batched_params;
                batched_params.SetParameters(frag_shader, params);

                g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);
                _ui_command_list->SetScissor(r);
                _ui_command_list->DrawIndexedInstanced(cmd->ElemCount, 1, cmd->IdxOffset, cmd->VtxOffset + global_vertex_offset, 0);
            }
        }
        global_index_offset += cmd_list->IdxBuffer.Size;
        global_vertex_offset += cmd_list->VtxBuffer.Size;
    }
}

void SetupRenderState(ImDrawData* draw_data, RHIGraphicsCommandList* commandList, GuiFrameRenderBuffers* render_buffers) {
    GuiBackendData* backend_data = GetBackendData();

    ImGuiShaderVert::Parameters param;
    std::memset(&param.vertexBuffer, 0, sizeof(param.vertexBuffer));
    {
        float l = draw_data->DisplayPos.x;
        float r = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float t = draw_data->DisplayPos.y;
        float b = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        float mvp[4][4] =
            {
                {2.0f / (r - l), 0.0f, 0.0f, 0.0f},
                {0.0f, 2.0f / (t - b), 0.0f, 0.0f},
                {0.0f, 0.0f, 0.5f, 0.0f},
                {(r + l) / (l - r), (t + b) / (b - t), 0.5f, 1.0f},
            };
        memcpy(&param.vertexBuffer.mvp, mvp, sizeof(mvp));
    }
    ViewPort view_port(0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.f, 1.f);
    commandList->SetViewPort(view_port);

    uint32_t offsets[] = {0};
    commandList->BindVertexBuffers(0, 1, &render_buffers->vertex_buffer, offsets);
    commandList->BindIndexBuffer(render_buffers->index_buffer.Get(), 0, EIndexElementType::IET_UINT16);

    RHIBatchedShaderParameters batched_params;

    batched_params.SetParameters(backend_data->shader_module_vert, param);

    g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

    commandList->SetPipelineState(backend_data->pipeline);
    //blend factor?
}

//
void InvalidateDeviceObjects() {
    GuiBackendData* bd = GetBackendData();
    if (!bd)
        return;

    ImGuiIO& io = ImGui::GetIO();

    //todo: destroy maybe?
    bd->pipeline->DeRef();
    bd->font_texture->DeRef();
    bd->font_view->DeRef();
    bd->font_sampler->DeRef();

    bd->shader_module_frag->DeRef();
    bd->shader_module_vert->DeRef();

    io.Fonts->SetTexID(0);// We copied bd->pFontTextureView to io.Fonts->TexID so let's clear that as well.
}
bool CreateDeviceObjects() {
    GuiBackendData* backend_data = GetBackendData();
    if (!backend_data)
        return false;
    if (backend_data->pipeline)
        InvalidateDeviceObjects();

    RHISamplerInitializer sampler_init(ESamplerFilter::SF_LINEAR, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    sampler_init.compare_op    = CO_ALWAYS;
    RHISamplerRef sampler      = g_rhi->RHICreateSampler(sampler_init);
    backend_data->font_sampler = sampler;

    RHIGraphicsPipelineStateInitializer pso_init;

    pso_init.color_attachment_formats[0] = backend_data->attachment_format;
    pso_init.color_attachment_flags[0]   = ETextureUsageFlags::COLOR_ATTACHMENT;
    pso_init.color_attachment_count      = pso_init.CalcValidColorAttachmentCount();

    auto& shader_stage_input = pso_init.shader_stage;

    // VertexElement
    VertexInputStateInitializerList input_intializer{};
    input_intializer[0] = VertexElement(0, IM_OFFSETOF(ImDrawVert, pos), PF_R32G32_SFLOAT, 0, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[1] = VertexElement(0, IM_OFFSETOF(ImDrawVert, uv), PF_R32G32_SFLOAT, 1, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[2] = VertexElement(0, IM_OFFSETOF(ImDrawVert, col), PF_R8G8B8A8_UNORM, 2, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);

    RHIVertexShaderRef     gui_vert    = g_rhi->RHICreateVertexShader(ShaderResourceManager::GetShader<ImGuiShaderVert>());
    RHIFragmentShaderRef   gui_frag    = g_rhi->RHICreateFragmentShader(ShaderResourceManager::GetShader<ImGuiShaderFrag>());
    RHIVertexInputStateRef input_state = g_rhi->RHICreateVertexInputState(input_intializer);

    shader_stage_input.p_vertex_input_state = input_state;

    shader_stage_input.p_vertex_shader   = gui_vert;
    shader_stage_input.p_fragment_shader = gui_frag;

    RHIBlendStateInitializer blend_state_info;
    blend_state_info.attachments[0].color_blend_op         = BO_ADD;
    blend_state_info.attachments[0].color_src_blend_factor = BF_SRC_COLOR;
    blend_state_info.attachments[0].color_src_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].alpha_blend_op         = BO_ADD;
    blend_state_info.attachments[0].alpha_src_blend_factor = BF_ONE;
    blend_state_info.attachments[0].alpha_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].color_write_mask       = CW_RGBA;

    auto blend_state = g_rhi->RHICreateBlendState(blend_state_info);

    pso_init.blend_state        = blend_state;
    pso_init.primitive_topology = EPrimitiveTopology::TRIANGLE_LIST;

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

    pso_init.multisample_state   = g_rhi->RHICreateMultiSampleState(msaa_init);
    pso_init.rasterizer_state    = g_rhi->RHICreateRasterizationState(rast_init);
    pso_init.depth_stencil_state = g_rhi->RHICreateDepthStencilState(depth_stencil_init);

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
        uint32_t upload_size  = 4 * height * upload_pitch;

        RHIBufferRef staging_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(upload_size, 0, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));

        assert(font_texture.Get() && staging_buffer.Get());

        void* mapped = g_rhi->RHIMapBuffer(staging_buffer, 0, upload_size);
        for (int32_t y = 0; y < height; y++) {
            memcpy((void*)((uintptr_t)mapped + y * upload_pitch), pixels + y * width * 4, width * 4);
        }
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

        RHIGraphicsCommandList* command_list = g_rhi->CreateGraphicsCommandList();

        RHIBarrierDependencyInfo font_create_barriers{};
        font_create_barriers.texture_barrier_count = 1;
        font_create_barriers.p_texture_barriers    = tex_barriers;

        command_list->Open();
        command_list->SetPipelineBarrier(font_create_barriers);

        RHISubresourceSlice        resource_slice(ETextureAspectFlags::COLOR, 0, 0, 1, 0, 1);
        RHICopyBufferToTextureInfo copy_info(
            ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST,
            {0, 0, 0},
            {(uint32_t)width, (uint32_t)height, 1},
            resource_slice,
            0,
            upload_pitch,
            height);

        // 3. MARK... pRegion[0] is trying to copy 518144 bytes plus 0 offset to/from the VkBuffer (VkBuffer 0xcb1c7c000000001b[]) which exceeds the VkBuffer total size of 131072 bytes.
        command_list->CopyBufferToTexture(staging_buffer, font_texture, copy_info);

        RHIBarrierDependencyInfo font_copy_barriers{};
        font_copy_barriers.p_texture_barriers    = &tex_barriers[1];
        font_copy_barriers.texture_barrier_count = 1;

        command_list->SetPipelineBarrier(font_copy_barriers);

        RHIBatchedShaderParameters  batched_params;
        ImGuiShaderFrag::Parameters params;
        params.sampler0 = backend_data->font_sampler;
        params.texture0 = backend_data->font_view;

        batched_params.SetParameters(backend_data->shader_module_frag, params);
        g_rhi->RHISetBatchedShaderParameters(backend_data->pipeline, batched_params);

        command_list->Close();

        RHICommandQueue* queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);

        RHIFenceCreateInfo fence_info{EFenceUsage::TIMELINE};
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
void GuiSwapbuffer(ImGuiViewport* viewport, void*);

void GuiInitPlatformInterface() {
    ImGuiPlatformIO& platform_io       = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow  = GuiCreateWindow;
    platform_io.Renderer_DestroyWindow = GuiDestroyWindow;
    platform_io.Renderer_SetWindowSize = GuiSetWindowSize;
    platform_io.Renderer_RenderWindow  = GuiRenderWindow;
    platform_io.Renderer_SwapBuffers   = GuiSwapbuffer;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* _render_buffers) {
    _render_buffers->index_buffer->DeRef();
    _render_buffers->vertex_buffer->DeRef();
}

void GuiCreateWindow(ImGuiViewport* viewport) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = IM_NEW(GuiViewportData)();

    viewport->RendererUserData = viewport_data;

    viewport_data->command_queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);

    viewport_data->comand_list = g_rhi->CreateGraphicsCommandList();

    viewport_data->comand_list->Close();

    RHIFenceCreateInfo present_fence_info{EFenceUsage::PRESENT};
    viewport_data->present_fence = g_rhi->RHICreateFence(present_fence_info);

    RHIViewportInitializer viewport_info;
    viewport_data->frame_index = 0;

    viewport_data->viewport = g_rhi->RHICreateViewport(viewport_info);
}

void GuiDestroyWindow(ImGuiViewport* viewport) {
    GuiBackendData* backend_data = GetBackendData();

    if (GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData) {
        if (viewport_data && viewport_data->command_queue && viewport_data->present_fence) {
            viewport_data->viewport->WaitForQueueComplete(viewport_data->command_queue, viewport_data->present_fence);

            delete viewport_data->comand_list;
            viewport_data->comand_list = nullptr;
            delete viewport_data->command_queue;
            viewport_data->command_queue = nullptr;

            viewport_data->present_fence->DeRef();

            uint32_t max_frame = viewport_data->viewport->GetViewportInfo().max_frame_in_flight;

            for (uint32_t index = 0; index < max_frame; index++) {
                viewport_data->render_buffers[index].index_buffer->DeRef();
                viewport_data->render_buffers[index].vertex_buffer->DeRef();
            }

            viewport_data->viewport->DeRef();
            IM_DELETE(viewport_data);
        }
    }
    viewport->RendererUserData = nullptr;
}
void GuiSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;

    auto m_viewport = viewport_data->viewport;

    m_viewport->WaitForQueueComplete(viewport_data->command_queue, viewport_data->present_fence);

    m_viewport->OnResize(Extent2D(size.x, size.y));
}
void GuiRenderWindow(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;
    RHIViewport*     rhi_viewport  = viewport_data->viewport;

    RHIViewportNextBackBufferInfo info = g_rhi->RHIGetNextFrameViewportBufferInfo(rhi_viewport);

    if (info.backbuffer_index == UINT32_MAX) return;

    RHIUnorderedAccessView* present_view = g_rhi->RHIGetViewportBackBufferUAV(rhi_viewport, info.backbuffer_index);
    if (present_view == nullptr) {
        //meet resize event
        return;
    }

    //transfer present texture layout to color attachment layout
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

    //transfer present texture layout to present src
    RHIBarrierDependencyInfo             texture_dependency_info;
    std::array<RHITextureBarrierInfo, 1> texture_barriers_present;
    texture_barriers_present[0].SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC);
    texture_barriers_present[0].SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT);
    texture_barriers_present[0].p_texture = present_view->GetTexture();
    texture_barriers_present[0].SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE);

    texture_dependency_info.texture_barrier_count = 1;
    texture_dependency_info.p_texture_barriers    = texture_barriers.data();

    viewport_data->comand_list->Reset();

    viewport_data->comand_list->Open();

    viewport_data->comand_list->SetPipelineBarrier(dependency_info);
    viewport_data->comand_list->SetPipelineBarrier(texture_dependency_info);

    RHIRenderPassInfo pass_info;
    pass_info.color_attachments[0].color_attachment_action               = AC_CLEAR_STORE;
    pass_info.color_attachments[0].color_attachment_view.texture_view    = present_view;
    pass_info.color_attachments[0].color_attachment_view.required_layout = ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT;

    viewport_data->comand_list->BeginRenderPass(pass_info, "Imgui Window");

    g_rhi->GUIRender(viewport->DrawData, viewport_data->comand_list);

    viewport_data->comand_list->EndRenderPass();

    viewport_data->comand_list->Close();

    RHISubmitInfo submit_info{};

    //wait for last frame recording
    submit_info.Wait(viewport_data->present_fence, viewport_data->frame_index);
    //wait for back_buffer ready
    submit_info.Wait(info.backbuffer_ready_fence, 0);
    //signal this frame present fence
    submit_info.Signal(viewport_data->present_fence, ++viewport_data->frame_index);

    viewport_data->command_queue->SubmitCommands(1, viewport_data->comand_list);
}

void GuiSwapbuffer(ImGuiViewport* viewport, void*) {
    GuiBackendData*  backend_data  = GetBackendData();
    GuiViewportData* viewport_data = (GuiViewportData*)viewport->RendererUserData;
    //present wait for this frame rendering end fence
    viewport_data->viewport->Present(viewport_data->present_fence);
}

static void GuiRenderWindows() {
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    for (int i = 1; i < platform_io.Viewports.Size; i++)
        if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
            GuiRenderWindow(platform_io.Viewports[i], nullptr);
    for (int i = 1; i < platform_io.Viewports.Size; i++)
        if ((platform_io.Viewports[i]->Flags & ImGuiViewportFlags_IsMinimized) == 0)
            GuiSwapbuffer(platform_io.Viewports[i], nullptr);
}
