
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

#include <imgui.h>
#include <vadefs.h>
#include <vcruntime_string.h>

class ImGuiShaderVert : public Shader {
    DEFINE_SHADER_TYPE(ImGuiShaderVert, Global, RHI_API, ...)
public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(UIVertex)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, ProjectionMatrix)
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

void GuiInitPlatformInterface();

inline GuiBackendData* GetBackendData() {
    return ImGui::GetCurrentContext() ? (GuiBackendData*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* buffer);

bool CreateDeviceObjects();
void CreateFontsTexture();
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
    ImGuiViewport* main_viewport    = ImGui::GetMainViewport();
    main_viewport->RendererUserData = IM_NEW(GuiViewportData)(render_backend_data->num_frames_in_flight);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        GuiInitPlatformInterface();

    // render_backend_data->
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
void RHI::GUIRender() {
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
    io.Fonts->SetTexID(0);// We copied bd->pFontTextureView to io.Fonts->TexID so let's clear that as well.
}
bool CreateDeviceObjects() {
    GuiBackendData* bd = GetBackendData();
    if (!bd)
        return false;
    if (bd->pipeline)
        InvalidateDeviceObjects();

    RHISamplerInitializer sampler_init(ESamplerFilter::SF_LINEAR);
    sampler_init.compare_op = CO_ALWAYS;
    RHISamplerRef sampler   = g_rhi->RHICreateSampler(sampler_init);

    RHIGraphicsPipelineStateInitializer pso_init;

    pso_init.color_attachment_formats[0] = bd->attachment_format;
    pso_init.color_attachment_flags[0]   = ETextureUsageFlags::COLOR_ATTACHMENT;
    pso_init.color_attachment_count      = pso_init.CalcValidColorAttachmentCount();

    auto& shader_stage_input = pso_init.shader_stage;

    // VertexElement
    VertexInputStateInitializerList input_intializer{};
    input_intializer[0] = VertexElement(0, IM_OFFSETOF(ImDrawVert, pos), PF_R32G32_SFLOAT, 0, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[1] = VertexElement(0, IM_OFFSETOF(ImDrawVert, uv), PF_R32G32_SFLOAT, 1, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);
    input_intializer[1] = VertexElement(0, IM_OFFSETOF(ImDrawVert, col), PF_R8G8B8A8_UNORM, 2, sizeof(ImDrawVert), EVertexInputRate::VIR_VERTEX);

    RHIShaderRef gui_vert = g_rhi->RHICreateShader(ShaderResourceManager::GetShader<ImGuiShaderVert>());
    RHIShaderRef gui_frag = g_rhi->RHICreateShader(ShaderResourceManager::GetShader<ImGuiShaderFrag>());

    shader_stage_input.p_vertex_input_state = g_rhi->RHICreateVertexInputState(input_intializer);

    shader_stage_input.p_vertex_shader   = (RHIVertexShader*)gui_vert.Get();
    shader_stage_input.p_fragment_shader = (RHIFragmentShader*)gui_frag.Get();

    RHIBlendStateInitializer blend_state_info;
    blend_state_info.attachments[0].color_blend_op         = BO_ADD;
    blend_state_info.attachments[0].color_src_blend_factor = BF_SRC_COLOR;
    blend_state_info.attachments[0].color_src_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].alpha_blend_op         = BO_ADD;
    blend_state_info.attachments[0].alpha_src_blend_factor = BF_ONE;
    blend_state_info.attachments[0].alpha_dst_blend_factor = BF_ONE_MINUS_SRC_ALPHA;
    blend_state_info.attachments[0].color_write_mask       = CW_RGBA;

    auto blend_state = g_rhi->RHICreateBlendState(blend_state_info);

    pso_init.blend_state = blend_state;

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

    bd->pipeline = g_rhi->RHICreateGraphicsPipelineState(pso_init);

    // ImGui_ImplDX12_CreateFontsTexture();

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

        font_texture          = g_rhi->RHICreateTexture(RHITextureCreateInfo::Create("FontTexture2D", ETextureDimension::TEX_2D)
                                                   .SetNumSamples(1)
                                                   .SetExtent({width, height})
                                                   .SetNumMips(1)
                                                   .SetArraySize(1)
                                                   .SetFormat(PF_R8G8B8A8_UNORM)
                                                   .SetUsageFlags(ETextureUsageFlags::SHADER_RESOURCE | ETextureUsageFlags::SRGB)
                                                   .SetInitialLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED));
        uint32_t upload_pitch = (width * 4 + alignment - 1u) & ~(alignment - 1u);
        uint32_t upload_size  = height * upload_pitch;

        RHIBufferRef staging_buffer = g_rhi->RHICreateBuffer(
            RHIBufferCreateInfo::Create(upload_size, 0, EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::CPU_VISIBLE));

        assert(font_texture.Get() && staging_buffer.Get());

        void* mapped = g_rhi->RHIMapBuffer(staging_buffer, 0, upload_size);
        for (int32_t y = 0; y < height; y++) {
            memcpy((void*)((uintptr_t)mapped + y * upload_pitch), pixels + y * width * 4, width * 4);
        }
        g_rhi->RHIUnmapBuffer(staging_buffer);

        RHISubresourceRange range{ETextureAspectFlags::COLOR};

        RHITextureBarrierInfo tex_barriers[2];

        tex_barriers[0].src_layout         = TEXTURE_LAYOUT_UNDEFINED;
        tex_barriers[0].dst_layout         = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[0].src_access         = ERHIAccessFlags::UNDEFINED;
        tex_barriers[0].dst_access         = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[0].p_texture          = font_texture;
        tex_barriers[0].sub_resource_range = range;

        tex_barriers[1].src_layout         = TEXTURE_LAYOUT_TRANSFER_DST;
        tex_barriers[1].dst_layout         = TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tex_barriers[1].src_access         = ERHIAccessFlags::TRANSFER_WRITE;
        tex_barriers[1].dst_access         = ERHIAccessFlags::SHADER_READ;
        tex_barriers[1].p_texture          = font_texture;
        tex_barriers[1].sub_resource_range = range;

        RHIGraphicsCommandList* command_list = g_rhi->CreateGraphicsCommandList();

        RHIBarrierDependencyInfo font_create_barriers{};
        font_create_barriers.texture_barrier_count = 1;
        font_create_barriers.p_texture_barriers    = tex_barriers;

        command_list->Open();
        command_list->SetPipelineBarrier(font_create_barriers);

        RHISubresourceSlice        resource_slice(ETextureAspectFlags::COLOR, 0, 0);
        RHICopyBufferToTextureInfo copy_info(
            ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST,
            {0, 0, 0},
            {(uint32_t)width, (uint32_t)height, 1},
            resource_slice,
            0,
            upload_pitch,
            height);

        command_list->CopyBufferToTexture(staging_buffer, font_texture, copy_info);

        RHIBarrierDependencyInfo font_copy_barriers{};
        font_copy_barriers.p_texture_barriers    = &tex_barriers[1];
        font_copy_barriers.texture_barrier_count = 1;

        command_list->SetPipelineBarrier(font_copy_barriers);

        command_list->Close();

        RHICommandQueue* queue = g_rhi->CreateCommandQueue(ECommandQueueType::GRAPHICS);
        queue->SubmitCommands(1, command_list);

        RHIFenceRef fence = g_rhi->RHICreateFence("font_tex_creation");
    }
}
void GuiInitPlatformInterface() {
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* buffer) {
}