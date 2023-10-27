
#include "PixelFormat.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "RHIUIImplement.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"

#include <imgui.h>

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

    // // Create the root signature
    // {
    //     D3D12_DESCRIPTOR_RANGE descRange            = {};
    //     descRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    //     descRange.NumDescriptors                    = 1;
    //     descRange.BaseShaderRegister                = 0;
    //     descRange.RegisterSpace                     = 0;
    //     descRange.OffsetInDescriptorsFromTableStart = 0;

    //     D3D12_ROOT_PARAMETER param[2] = {};

    //     param[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    //     param[0].Constants.ShaderRegister = 0;
    //     param[0].Constants.RegisterSpace  = 0;
    //     param[0].Constants.Num32BitValues = 16;
    //     param[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    //     param[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    //     param[1].DescriptorTable.NumDescriptorRanges = 1;
    //     param[1].DescriptorTable.pDescriptorRanges   = &descRange;
    //     param[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    RHISamplerRef sampler;
    RHITextureRef texture;
    //     // Bilinear sampling is required by default. Set 'io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines' or 'style.AntiAliasedLinesUseTex = false' to allow point/nearest sampling.
    //     D3D12_STATIC_SAMPLER_DESC staticSampler = {};
    //     staticSampler.Filter                    = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    //     staticSampler.AddressU                  = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    //     staticSampler.AddressV                  = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    //     staticSampler.AddressW                  = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    //     staticSampler.MipLODBias                = 0.f;
    //     staticSampler.MaxAnisotropy             = 0;
    //     staticSampler.ComparisonFunc            = D3D12_COMPARISON_FUNC_ALWAYS;
    //     staticSampler.BorderColor               = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    //     staticSampler.MinLOD                    = 0.f;
    //     staticSampler.MaxLOD                    = 0.f;
    //     staticSampler.ShaderRegister            = 0;
    //     staticSampler.RegisterSpace             = 0;
    //     staticSampler.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    //     D3D12_ROOT_SIGNATURE_DESC desc = {};
    //     desc.NumParameters             = _countof(param);
    //     desc.pParameters               = param;
    //     desc.NumStaticSamplers         = 1;
    //     desc.pStaticSamplers           = &staticSampler;
    //     desc.Flags =
    //         D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
    //         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
    //         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
    //         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    //     // Load d3d12.dll and D3D12SerializeRootSignature() function address dynamically to facilitate using with D3D12On7.
    //     // See if any version of d3d12.dll is already loaded in the process. If so, give preference to that.
    //     static HINSTANCE d3d12_dll = ::GetModuleHandleA("d3d12.dll");
    //     if (d3d12_dll == nullptr) {
    //         // Attempt to load d3d12.dll from local directories. This will only succeed if
    //         // (1) the current OS is Windows 7, and
    //         // (2) there exists a version of d3d12.dll for Windows 7 (D3D12On7) in one of the following directories.
    //         // See https://github.com/ocornut/imgui/pull/3696 for details.
    //         const char* localD3d12Paths[] = {".\\d3d12.dll", ".\\d3d12on7\\d3d12.dll", ".\\12on7\\d3d12.dll"};// A. current directory, B. used by some games, C. used in Microsoft D3D12On7 sample
    //         for (int i = 0; i < IM_ARRAYSIZE(localD3d12Paths); i++)
    //             if ((d3d12_dll = ::LoadLibraryA(localD3d12Paths[i])) != nullptr)
    //                 break;

    //         // If failed, we are on Windows >= 10.
    //         if (d3d12_dll == nullptr)
    //             d3d12_dll = ::LoadLibraryA("d3d12.dll");

    //         if (d3d12_dll == nullptr)
    //             return false;
    //     }

    //     PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignatureFn = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)::GetProcAddress(d3d12_dll, "D3D12SerializeRootSignature");
    //     if (D3D12SerializeRootSignatureFn == nullptr)
    //         return false;

    //     ID3DBlob* blob = nullptr;
    //     if (D3D12SerializeRootSignatureFn(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr) != S_OK)
    //         return false;

    //     bd->pd3dDevice->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&bd->pRootSignature));
    //     blob->Release();
    // }

    // // By using D3DCompile() from <d3dcompiler.h> / d3dcompiler.lib, we introduce a dependency to a given version of d3dcompiler_XX.dll (see D3DCOMPILER_DLL_A)
    // // If you would like to use this DX12 sample code but remove this dependency you can:
    // //  1) compile once, save the compiled shader blobs into a file or source code and assign them to psoDesc.VS/PS [preferred solution]
    // //  2) use code to detect any version of the DLL and grab a pointer to D3DCompile from the DLL.
    // // See https://github.com/ocornut/imgui/pull/638 for sources and details.

    // D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
    // memset(&psoDesc, 0, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    // psoDesc.NodeMask              = 1;
    // psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // psoDesc.pRootSignature        = bd->pRootSignature;
    // psoDesc.SampleMask            = UINT_MAX;
    // psoDesc.NumRenderTargets      = 1;
    // psoDesc.RTVFormats[0]         = bd->RTVFormat;
    // psoDesc.SampleDesc.Count      = 1;
    // psoDesc.Flags                 = D3D12_PIPELINE_STATE_FLAG_NONE;
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
void GuiInitPlatformInterface() {
}

void DestroyRenderBuffers(GuiFrameRenderBuffers* buffer) {
}