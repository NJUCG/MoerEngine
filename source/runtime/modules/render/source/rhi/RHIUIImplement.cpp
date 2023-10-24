
#include "rhi/RHI.h"
#include "RHIUIImplement.h"

#include <imgui.h>
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
    if (GuiViewportData* vd = (GuiViewportData*)main_viewport->RendererUserData) {
        // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
        for (uint32_t i = 0; i < bd->num_frames_in_flight; i++)
            // DestroyRenderBuffers(&vd->render_buffers[i]);
            IM_DELETE(vd);
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

    // ID3DBlob* vertexShaderBlob;
    // ID3DBlob* pixelShaderBlob;

    // // Create the vertex shader
    // {
    //     static const char* vertexShader =
    //         "cbuffer vertexBuffer : register(b0) \
    //         {\
    //           float4x4 ProjectionMatrix; \
    //         };\
    //         struct VS_INPUT\
    //         {\
    //           float2 pos : POSITION;\
    //           float4 col : COLOR0;\
    //           float2 uv  : TEXCOORD0;\
    //         };\
    //         \
    //         struct PS_INPUT\
    //         {\
    //           float4 pos : SV_POSITION;\
    //           float4 col : COLOR0;\
    //           float2 uv  : TEXCOORD0;\
    //         };\
    //         \
    //         PS_INPUT main(VS_INPUT input)\
    //         {\
    //           PS_INPUT output;\
    //           output.pos = mul( ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));\
    //           output.col = input.col;\
    //           output.uv  = input.uv;\
    //           return output;\
    //         }";

    //     if (FAILED(D3DCompile(vertexShader, strlen(vertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vertexShaderBlob, nullptr)))
    //         return false;// NB: Pass ID3DBlob* pErrorBlob to D3DCompile() to get error showing in (const char*)pErrorBlob->GetBufferPointer(). Make sure to Release() the blob!
    //     psoDesc.VS = {vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize()};

    //     // Create the input layout
    //     static D3D12_INPUT_ELEMENT_DESC local_layout[] =
    //         {
    //             {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    //             {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)IM_OFFSETOF(ImDrawVert, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    //             {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    //         };
    //     psoDesc.InputLayout = {local_layout, 3};
    // }

    // // Create the pixel shader
    // {
    //     static const char* pixelShader =
    //         "struct PS_INPUT\
    //         {\
    //           float4 pos : SV_POSITION;\
    //           float4 col : COLOR0;\
    //           float2 uv  : TEXCOORD0;\
    //         };\
    //         SamplerState sampler0 : register(s0);\
    //         Texture2D texture0 : register(t0);\
    //         \
    //         float4 main(PS_INPUT input) : SV_Target\
    //         {\
    //           float4 out_col = input.col * texture0.Sample(sampler0, input.uv); \
    //           return out_col; \
    //         }";

    //     if (FAILED(D3DCompile(pixelShader, strlen(pixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixelShaderBlob, nullptr))) {
    //         vertexShaderBlob->Release();
    //         return false;// NB: Pass ID3DBlob* pErrorBlob to D3DCompile() to get error showing in (const char*)pErrorBlob->GetBufferPointer(). Make sure to Release() the blob!
    //     }
    //     psoDesc.PS = {pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize()};
    // }

    // // Create the blending setup
    // {
    //     D3D12_BLEND_DESC& desc                     = psoDesc.BlendState;
    //     desc.AlphaToCoverageEnable                 = false;
    //     desc.RenderTarget[0].BlendEnable           = true;
    //     desc.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    //     desc.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    //     desc.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    //     desc.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    //     desc.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    //     desc.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    //     desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // }

    // // Create the rasterizer state
    // {
    //     D3D12_RASTERIZER_DESC& desc = psoDesc.RasterizerState;
    //     desc.FillMode               = D3D12_FILL_MODE_SOLID;
    //     desc.CullMode               = D3D12_CULL_MODE_NONE;
    //     desc.FrontCounterClockwise  = FALSE;
    //     desc.DepthBias              = D3D12_DEFAULT_DEPTH_BIAS;
    //     desc.DepthBiasClamp         = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    //     desc.SlopeScaledDepthBias   = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    //     desc.DepthClipEnable        = true;
    //     desc.MultisampleEnable      = FALSE;
    //     desc.AntialiasedLineEnable  = FALSE;
    //     desc.ForcedSampleCount      = 0;
    //     desc.ConservativeRaster     = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    // }

    // // Create depth-stencil State
    // {
    //     D3D12_DEPTH_STENCIL_DESC& desc = psoDesc.DepthStencilState;
    //     desc.DepthEnable               = false;
    //     desc.DepthWriteMask            = D3D12_DEPTH_WRITE_MASK_ALL;
    //     desc.DepthFunc                 = D3D12_COMPARISON_FUNC_ALWAYS;
    //     desc.StencilEnable             = false;
    //     desc.FrontFace.StencilFailOp = desc.FrontFace.StencilDepthFailOp = desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    //     desc.FrontFace.StencilFunc                                                                      = D3D12_COMPARISON_FUNC_ALWAYS;
    //     desc.BackFace                                                                                   = desc.FrontFace;
    // }

    // HRESULT result_pipeline_state = bd->pd3dDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&bd->pPipelineState));
    // vertexShaderBlob->Release();
    // pixelShaderBlob->Release();
    // if (result_pipeline_state != S_OK)
    //     return false;

    // ImGui_ImplDX12_CreateFontsTexture();

    return true;
};
void GuiInitPlatformInterface() {
}