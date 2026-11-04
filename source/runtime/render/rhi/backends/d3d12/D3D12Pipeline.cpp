#include "D3D12Backend.h"
#include "rhi/ShaderStageUtils.h"
#include <algorithm>
#include <d3d12shader.h>
#include <dxcapi.h>

namespace Moer::Render::D3D12 {
namespace {
D3D12_SHADER_VISIBILITY Visibility(EShaderType stage) {
    switch (stage) {
        case ST_VERTEX:
            return D3D12_SHADER_VISIBILITY_VERTEX;
        case ST_FRAGMENT:
            return D3D12_SHADER_VISIBILITY_PIXEL;
        case ST_GEOMETRY:
            return D3D12_SHADER_VISIBILITY_GEOMETRY;
        case ST_HULL:
            return D3D12_SHADER_VISIBILITY_HULL;
        case ST_DOMAIN:
            return D3D12_SHADER_VISIBILITY_DOMAIN;
        case ST_MESH:
            return D3D12_SHADER_VISIBILITY_MESH;
        case ST_AMPLIFICATION:
            return D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        default:
            return D3D12_SHADER_VISIBILITY_ALL;
    }
}
bool IsUav(ED3D12ShaderVariableType type) {
    return (type >= ED3D12ShaderVariableType::RWByteAddressBuffer && type <= ED3D12ShaderVariableType::RWTypedBuffer) ||
           (type >= ED3D12ShaderVariableType::RWTexture1D && type <= ED3D12ShaderVariableType::RWTexture3D);
}
D3D12_BLEND Blend(EBlendFactor factor) {
    constexpr D3D12_BLEND values[] = {D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_SRC1_COLOR, D3D12_BLEND_INV_SRC1_COLOR, D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA};
    return values[uint32(factor)];
}
ComPtr<ID3D12ShaderReflection> Reflect(const SingleShaderInfo& shader) {
    ComPtr<IDxcUtils>               utils;
    ComPtr<IDxcBlobEncoding>        blob;
    ComPtr<IDxcContainerReflection> container;
    DX_CHECK_HRESULT(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)));
    DX_CHECK_HRESULT(utils->CreateBlob(shader.shader_data.data(), uint32(shader.shader_data.size()), 0, &blob));
    DX_CHECK_HRESULT(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container)));
    DX_CHECK_HRESULT(container->Load(blob.Get()));
    uint32 index = 0;
    DX_CHECK_HRESULT(container->FindFirstPartKind(0x4c495844, &index));
    ComPtr<ID3D12ShaderReflection> reflection;
    DX_CHECK_HRESULT(container->GetPartReflection(index, IID_PPV_ARGS(&reflection)));
    return reflection;
}
} // namespace

DXGI_FORMAT VertexFormat(EVertexFormat format) {
    switch (format) {
        case EVertexFormat::Float1:
            return DXGI_FORMAT_R32_FLOAT;
        case EVertexFormat::Float2:
            return DXGI_FORMAT_R32G32_FLOAT;
        case EVertexFormat::Float3:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case EVertexFormat::Float4:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case EVertexFormat::UInt:
            return DXGI_FORMAT_R32_UINT;
        case EVertexFormat::UByte4Normalized:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        default:
            throw std::invalid_argument("Invalid D3D12 vertex format");
    }
}

PipelineHandle CreatePipeline(Context& context, PipelineShaderInfo&& shaders, const GfxPsoCreateInfo* gfx) {
    const auto error = gfx ? ValidateGraphicsShaderStages(shaders.shaders) : ValidateComputeShaderStages(shaders.shaders);
    if (!error.empty())
        throw std::invalid_argument(std::string(error));
    if (gfx && gfx->view_mask)
        throw std::invalid_argument("D3D12 multiview is not enabled");
    const auto arguments = shaders.argument_metadata.GetArguments();
    if (arguments.size() > 64)
        throw std::invalid_argument("D3D12 pipelines support at most 64 logical arguments");
    uint64                         active = 0;
    Array<Binding>                 bindings;
    Array<D3D12_ROOT_PARAMETER1>   roots;
    Array<D3D12_DESCRIPTOR_RANGE1> ranges;
    ranges.reserve(shaders.shaders.size() * arguments.size());
    for (const auto& shader : shaders.shaders) {
        if (!shader.shader_param_map)
            throw std::invalid_argument("D3D12 requires DXIL reflection");
        std::set<std::pair<uint32, uint32>> constant_registers;
        for (uint32 arg_index = 0; arg_index < arguments.size(); ++arg_index) {
            const auto&       argument = arguments[arg_index];
            const std::string name(argument.cpp_info.type == SDA_BindlessArray ? ReflectParamInfo::bdls_name : argument.name);
            const auto&       map   = shader.shader_param_map->reflect_map;
            const auto        found = map.find(name);
            if (found == map.end())
                continue;
            const auto& reflection = found->second.GetDxilReflection();
            active |= uint64(1) << arg_index;
            Binding               binding{.argument = arg_index, .root = uint32(roots.size()), .count = std::max(1u, reflection.count), .type = ED3D12ShaderVariableType(reflection.type)};
            D3D12_ROOT_PARAMETER1 root{};
            root.ShaderVisibility = Visibility(shader.shader_type);
            if (argument.cpp_info.type == SDA_Constant) {
                if (!constant_registers.emplace(reflection.slot, reflection.space).second)
                    continue;
                binding.constants     = true;
                binding.constant_size = reflection.byte_size;
                for (const auto& layout : shaders.argument_metadata.GetConstantLayouts()) {
                    const auto member = map.find(std::string(arguments[layout.argument_index].name));
                    if (member == map.end())
                        continue;
                    const auto& value = member->second.GetDxilReflection();
                    if (value.slot != reflection.slot || value.space != reflection.space)
                        continue;
                    if (value.byte_offset + layout.byte_size > reflection.byte_size)
                        throw std::invalid_argument("DXIL constant argument exceeds cbuffer");
                    binding.constant_copies.push_back({layout.byte_offset, value.byte_offset, layout.byte_size});
                }
                root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                root.Descriptor    = {reflection.slot, reflection.space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            } else if (argument.cpp_info.type == SDA_BindlessArray) {
                root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                root.Descriptor    = {reflection.slot, reflection.space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            } else {
                root.ParameterType                           = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                const D3D12_DESCRIPTOR_RANGE_TYPE range_type = binding.type == ED3D12ShaderVariableType::Sampler        ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER :
                                                               binding.type == ED3D12ShaderVariableType::ConstantBuffer ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV :
                                                               IsUav(binding.type)                                      ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV :
                                                                                                                          D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges.push_back({range_type, binding.count, reflection.slot, reflection.space, range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE : D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0});
                root.DescriptorTable = {1, &ranges.back()};
            }
            roots.push_back(root);
            bindings.push_back(std::move(binding));
        }
    }
    uint32 dwords = 0;
    for (const auto& root : roots)
        dwords += root.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 1 : 2;
    if (dwords > 64)
        throw std::invalid_argument("D3D12 root signature exceeds 64 DWORDs");
    auto*          pipeline = MoerNew(Pipeline)(active);
    PipelineHandle owner{pipeline};
    pipeline->bindings = std::move(bindings);
    pipeline->compute  = gfx == nullptr;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature{};
    signature.Version  = D3D_ROOT_SIGNATURE_VERSION_1_1;
    signature.Desc_1_1 = {uint32(roots.size()), roots.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED};
    ComPtr<ID3DBlob> blob, errors;
    const HRESULT    serialized = D3D12SerializeVersionedRootSignature(&signature, &blob, &errors);
    if (FAILED(serialized) && errors)
        LOG_ERROR("D3D12 root signature: {}", static_cast<const char*>(errors->GetBufferPointer()));
    DX_CHECK_HRESULT(serialized);
    DX_CHECK_HRESULT(context.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pipeline->signature)));
    if (!gfx) {
        const auto&                       shader = shaders.shaders.front();
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = pipeline->signature.Get();
        desc.CS             = {shader.shader_data.data(), shader.shader_data.size()};
        DX_CHECK_HRESULT(context.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline->state)));
        return owner;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pipeline->signature.Get();
    ComPtr<ID3D12ShaderReflection> vertex_reflection;
    D3D12_SHADER_BYTECODE          mesh{}, amplification{};
    for (const auto& shader : shaders.shaders) {
        const D3D12_SHADER_BYTECODE code{shader.shader_data.data(), shader.shader_data.size()};
        switch (shader.shader_type) {
            case ST_VERTEX:
                desc.VS           = code;
                vertex_reflection = Reflect(shader);
                break;
            case ST_FRAGMENT:
                desc.PS = code;
                break;
            case ST_GEOMETRY:
                desc.GS = code;
                break;
            case ST_HULL:
                desc.HS = code;
                break;
            case ST_DOMAIN:
                desc.DS = code;
                break;
            case ST_MESH:
                mesh           = code;
                pipeline->mesh = true;
                break;
            case ST_AMPLIFICATION:
                amplification = code;
                break;
            default:
                break;
        }
    }
    Array<D3D12_SIGNATURE_PARAMETER_DESC> inputs;
    if (vertex_reflection) {
        D3D12_SHADER_DESC shader_desc{};
        vertex_reflection->GetDesc(&shader_desc);
        for (uint32 i = 0; i < shader_desc.InputParameters; ++i) {
            D3D12_SIGNATURE_PARAMETER_DESC input{};
            vertex_reflection->GetInputParameterDesc(i, &input);
            if (input.SystemValueType == D3D_NAME_UNDEFINED)
                inputs.push_back(input);
        }
    }
    Array<D3D12_INPUT_ELEMENT_DESC> elements;
    uint32                          location = 0;
    for (uint32 slot = 0; slot < gfx->vertex_stream.bindings.size(); ++slot) {
        const auto& binding = gfx->vertex_stream.bindings[slot];
        uint32      offset  = 0;
        for (const auto& element : binding.vertex_elements) {
            if (location >= inputs.size())
                throw std::invalid_argument("D3D12 vertex input count mismatch");
            const auto& input    = inputs[location++];
            const bool  instance = binding.input_rate == VIR_INSTANCE;
            elements.push_back({input.SemanticName, input.SemanticIndex, VertexFormat(element.format), slot, offset, instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, instance ? 1u : 0u});
            offset += GetVertexFormatByteSize(element.format);
        }
        pipeline->vertex_strides.push_back(offset);
    }
    desc.InputLayout                           = {elements.data(), uint32(elements.size())};
    const auto& raster                         = gfx->rasterizer_info;
    desc.RasterizerState                       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    desc.RasterizerState.FillMode              = raster.fill_mode == FM_LINE ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode              = raster.cull_mode == RCM_BACK ? D3D12_CULL_MODE_BACK : raster.cull_mode == RCM_FRONT ? D3D12_CULL_MODE_FRONT :
                                                                                                                                       D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = raster.b_front_counter_clockwise;
    desc.RasterizerState.DepthClipEnable       = !raster.b_depth_clamp_enable;
    desc.RasterizerState.DepthBias             = int32(raster.depth_bias);
    desc.RasterizerState.DepthBiasClamp        = raster.depth_bias_clamp;
    desc.RasterizerState.SlopeScaledDepthBias  = raster.depth_bias_slop_factor;
    const auto& depth                          = gfx->depth_stencil_info;
    desc.DepthStencilState                     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    desc.DepthStencilState.DepthEnable         = gfx->depth_stencil_format != PF_UNDEFINED;
    desc.DepthStencilState.DepthWriteMask      = depth.b_enable_depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc           = D3D12_COMPARISON_FUNC(uint32(depth.depth_test_op) + 1);
    desc.DepthStencilState.StencilEnable       = depth.b_enable_front_face_stencil || depth.b_enable_back_face_stencil;
    desc.DepthStencilState.StencilReadMask     = depth.stencil_readmask;
    desc.DepthStencilState.StencilWriteMask    = depth.stencil_writemask;
    desc.DepthStencilState.FrontFace           = {D3D12_STENCIL_OP(uint32(depth.front_face_stencil_fail_stencil_op) + 1), D3D12_STENCIL_OP(uint32(depth.front_face_depth_fail_stencil_op) + 1), D3D12_STENCIL_OP(uint32(depth.front_face_pass_stencil_op) + 1), D3D12_COMPARISON_FUNC(uint32(depth.front_face_stencil_test) + 1)};
    desc.DepthStencilState.BackFace            = {D3D12_STENCIL_OP(uint32(depth.back_face_stencil_fail_stencil_op) + 1), D3D12_STENCIL_OP(uint32(depth.back_face_depth_fail_stencil_op) + 1), D3D12_STENCIL_OP(uint32(depth.back_face_pass_stencil_op) + 1), D3D12_COMPARISON_FUNC(uint32(depth.back_face_stencil_test) + 1)};
    desc.DSVFormat                             = Format(gfx->depth_stencil_format);
    desc.BlendState                            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    desc.BlendState.AlphaToCoverageEnable      = gfx->multisample_info.b_alpha_to_converge;
    desc.BlendState.IndependentBlendEnable     = true;
    desc.NumRenderTargets                      = gfx->color_attachment_count;
    for (uint32 i = 0; i < desc.NumRenderTargets; ++i) {
        const auto& blend = gfx->color_attachments_info[i].blend_state_info;
        auto&       rt    = desc.BlendState.RenderTarget[i];
        rt.BlendEnable    = blend.color_src_blend_factor != BF_ONE || blend.color_dst_blend_factor != BF_ZERO ||
                         blend.alpha_src_blend_factor != BF_ONE || blend.alpha_dst_blend_factor != BF_ZERO;
        rt.SrcBlend              = Blend(blend.color_src_blend_factor);
        rt.DestBlend             = Blend(blend.color_dst_blend_factor);
        rt.BlendOp               = D3D12_BLEND_OP(uint32(blend.color_blend_op) + 1);
        rt.SrcBlendAlpha         = Blend(blend.alpha_src_blend_factor);
        rt.DestBlendAlpha        = Blend(blend.alpha_dst_blend_factor);
        rt.BlendOpAlpha          = D3D12_BLEND_OP(uint32(blend.alpha_blend_op) + 1);
        rt.RenderTargetWriteMask = uint8(blend.color_write_mask);
        desc.RTVFormats[i]       = Format(gfx->color_attachments_info[i].pixel_format);
    }
    constexpr D3D_PRIMITIVE_TOPOLOGY topologies[] = {D3D_PRIMITIVE_TOPOLOGY_POINTLIST, D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ};
    if (gfx->primitive_topology == EPrimitiveTopology::PATCH_LIST) {
        if (!gfx->patch_control_points || gfx->patch_control_points > 32)
            throw std::invalid_argument("D3D12 patch size must be 1..32");
        pipeline->topology         = D3D_PRIMITIVE_TOPOLOGY(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + gfx->patch_control_points - 1);
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    } else {
        pipeline->topology         = topologies[uint32(gfx->primitive_topology)];
        desc.PrimitiveTopologyType = gfx->primitive_topology == EPrimitiveTopology::POINT_LIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT :
                                     gfx->primitive_topology <= EPrimitiveTopology::LINE_STRIP ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE :
                                                                                                 D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    desc.SampleDesc.Count = gfx->multisample_info.sample_count;
    desc.SampleMask       = UINT32_MAX;
    if (pipeline->mesh) {
        if (!context.mesh_shaders)
            throw std::runtime_error("Adapter does not support mesh shaders");
        struct Stream {
            CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        root;
            CD3DX12_PIPELINE_STATE_STREAM_AS                    as;
            CD3DX12_PIPELINE_STATE_STREAM_MS                    ms;
            CD3DX12_PIPELINE_STATE_STREAM_PS                    ps;
            CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            blend;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK           mask;
            CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            raster;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL         depth;
            CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY    topology;
            CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS formats;
            CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT  depth_format;
            CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC           samples;
        } stream;
        stream.root     = desc.pRootSignature;
        stream.as       = amplification;
        stream.ms       = mesh;
        stream.ps       = desc.PS;
        stream.blend    = CD3DX12_BLEND_DESC(desc.BlendState);
        stream.mask     = desc.SampleMask;
        stream.raster   = CD3DX12_RASTERIZER_DESC(desc.RasterizerState);
        stream.depth    = CD3DX12_DEPTH_STENCIL_DESC(desc.DepthStencilState);
        stream.topology = desc.PrimitiveTopologyType;
        D3D12_RT_FORMAT_ARRAY formats{};
        formats.NumRenderTargets = desc.NumRenderTargets;
        std::copy(std::begin(desc.RTVFormats), std::end(desc.RTVFormats), formats.RTFormats);
        stream.formats      = formats;
        stream.depth_format = desc.DSVFormat;
        stream.samples      = desc.SampleDesc;
        const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
        DX_CHECK_HRESULT(context.device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline->state)));
    } else {
        const HRESULT result = context.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline->state));
        if (FAILED(result))
            for (const auto& shader : shaders.shaders)
                LOG_ERROR("D3D12 failing graphics stage: {} / {}", shader.name, shader.entry_point);
        DX_CHECK_HRESULT(result);
    }
    return owner;
}
} // namespace Moer::Render::D3D12
