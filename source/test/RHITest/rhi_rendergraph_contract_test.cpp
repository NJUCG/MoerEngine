#include "log/LogSystem.h"
#include "math/Matrix.h"
#include "misc/Traits.h"
#include "renderer/raytracing/GBufferPass.h"
#include "renderer/raytracing/VisualizePass.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"
#include "shaderheaders/shared/utils/Packing.h"

#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace Moer::Render::Tests {
namespace {

struct RGExecutionParams {
    DEFINE_RG_TEXTURE_ACCESS(texture, ETextureState::RENDER_TARGET);
    DEFINE_RG_BUFFER_ACCESS(buffer, EBufferState::UNORDERED_ACCESS);
    uint32_t prepared_value{0};
    uint32_t execution_count{0};
    bool     resources_allocated{false};

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer);
};

struct RGNestedReadParams {
    DEFINE_RG_TEXTURE_ACCESS(texture, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(buffer, EBufferState::SHADER_RESOURCE);

    DEFINE_RG_PARAMETER_ACCESS(texture, buffer);
};

struct RGAccessArrayParams {
    DEFINE_RG_NESTED_PARAMETER(RGNestedReadParams, nested);
    DEFINE_RG_TEXTURE_ACCESS_ARRAY(textures);
    DEFINE_RG_BUFFER_ACCESS_ARRAY(buffers);

    DEFINE_RG_PARAMETER_ACCESS(nested, textures, buffers);
};

struct RGSerialParams {
    uint32_t execution_count{0};
};

struct RGTextureUploadParams {
    DEFINE_RG_TEXTURE_ACCESS(target, ETextureState::TRANSFER_DST);

    DEFINE_RG_PARAMETER_ACCESS(target);
};

struct RGTextureCopyParams {
    RGTextureView src{};
    RGTextureView dst{};
    DEFINE_RG_TEXTURE_ACCESS_ARRAY(accesses);

    DEFINE_RG_PARAMETER_ACCESS(accesses);
};

struct RGBufferUploadParams {
    DEFINE_RG_BUFFER_ACCESS(target, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(target);
};

struct RGBufferDispatchParams {
    DEFINE_RG_BUFFER_ACCESS(source, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(output, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(source, output);
};

struct RGBufferReadbackStateParams {
    DEFINE_RG_BUFFER_ACCESS(source, EBufferState::TRANSFER_SRC);
    DEFINE_RG_PARAMETER_ACCESS(source);
};

struct RGBindlessUpdateParams {};

struct RGRayGeometryUploadParams {
    DEFINE_RG_BUFFER_ACCESS(vertices, EBufferState::TRANSFER_DST);
    DEFINE_RG_BUFFER_ACCESS(indices, EBufferState::TRANSFER_DST);
    DEFINE_RG_BUFFER_ACCESS(output, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(vertices, indices, output);
};

struct RGRayBuildBlasParams {
    DEFINE_RG_BUFFER_ACCESS(vertices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT);
    DEFINE_RG_BUFFER_ACCESS(indices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT);
    DEFINE_RG_BUFFER_ACCESS(blas_buffer, EBufferState::ACCELERATION_STRUCTURE_WRITE);
    DEFINE_RG_PARAMETER_ACCESS(vertices, indices, blas_buffer);
};

struct RGRayTlasParams {
    DEFINE_RG_BUFFER_ACCESS(blas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_WRITE);
    DEFINE_RG_PARAMETER_ACCESS(blas_buffer, tlas_buffer);
};

struct RGRayTraceParams {
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_BUFFER_ACCESS(output, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(tlas_buffer, output);
};

struct RGRaySceneUploadParams {
    DEFINE_RG_BUFFER_ACCESS_ARRAY(buffers);
    DEFINE_RG_PARAMETER_ACCESS(buffers);
};

struct RGRaySceneTraceParams {
    DEFINE_RG_BUFFER_ACCESS_ARRAY(scene_buffers);
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_BUFFER_ACCESS(output, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(scene_buffers, tlas_buffer, output);
};

struct RGRayGBufferTraceParams {
    DEFINE_RG_BUFFER_ACCESS_ARRAY(scene_buffers);
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_albedo, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(clip_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(tlas_buffer, EBufferState::ACCELERATION_STRUCTURE_READ);
    DEFINE_RG_PARAMETER_ACCESS(
        scene_buffers,
        constants,
        view_depth,
        diffuse_albedo,
        specular_roughness,
        normal,
        emission,
        motion,
        clip_depth,
        tlas_buffer
    );
};

struct RGGBufferTextureReadbackStateParams {
    DEFINE_RG_TEXTURE_ACCESS_ARRAY(textures);
    DEFINE_RG_PARAMETER_ACCESS(textures);
};

struct RGGBufferVisualizeParams {
    DEFINE_RG_BUFFER_ACCESS(constants, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(clip_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(prev_view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(output, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(
        constants,
        ldr_color,
        diffuse_lighting,
        specular_lighting,
        view_depth,
        clip_depth,
        emission,
        normal,
        specular_roughness,
        motion,
        normal_roughness,
        prev_view_depth,
        output
    );
};

struct RGRayPreparedTlasUpdate {
    UniquePtr<Command> command{};
};

struct RGBindlessDispatchArgs {
    uint32_t src_handle;
    uint32_t xor_mask;
    uint32_t element_count;
    uint32_t pad;
};

class RGBindlessDispatchPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RGBindlessDispatchPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(RGBindlessDispatchArgs, args);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(args, output_buffer, bdls);
};

class RGRayQueryHitPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RGRayQueryHitPipeline);

    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BUFFER(output_buffer);

    DEFINE_SHADER_ARGS(tlas, output_buffer);
};

class RGRaySceneBindlessPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RGRaySceneBindlessPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(GBufferPassParams, param);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(param, tlas, output_buffer, bdls);
};

bool HasHazard(
    const RGCompiledPlan& plan,
    uint32_t              src_pass,
    uint32_t              dst_pass,
    const RGResource*     resource,
    ERGResourceKind       resource_kind
) {
    for (const RGCompiledHazardEdge& edge : plan.hazard_edges) {
        if (edge.src_pass == src_pass && edge.dst_pass == dst_pass && edge.resource == resource &&
            edge.resource_kind == resource_kind) {
            return true;
        }
    }
    return false;
}

const RGCompiledHazardEdge* FindHazard(
    const RGCompiledPlan& plan,
    uint32_t              src_pass,
    uint32_t              dst_pass,
    const RGResource*     resource,
    ERGResourceKind       resource_kind
) {
    for (const RGCompiledHazardEdge& edge : plan.hazard_edges) {
        if (edge.src_pass == src_pass && edge.dst_pass == dst_pass && edge.resource == resource &&
            edge.resource_kind == resource_kind) {
            return &edge;
        }
    }
    return nullptr;
}

bool ValidatePassDomainHelpers() {
    if (!RGPassHasValidQueueFlags(ERGPassFlags::None) || !RGPassHasQueue(ERGPassFlags::Graphics) ||
        !RGPassHasQueue(ERGPassFlags::Compute) || !RGPassHasQueue(ERGPassFlags::Copy)) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass queue flags were rejected"));
        return false;
    }
    if (RGPassHasQueue(ERGPassFlags::None) ||
        RGPassHasValidQueueFlags(ERGPassFlags::Graphics | ERGPassFlags::Compute)) {
        LOG_ERROR(MOER_TEXT("Invalid RenderGraph pass queue flags were accepted"));
        return false;
    }
    if (RGPassQueue(ERGPassFlags::None) != EQueueType::Ignore ||
        RGPassQueue(ERGPassFlags::Graphics) != EQueueType::Graphics ||
        RGPassQueue(ERGPassFlags::Compute) != EQueueType::Compute ||
        RGPassQueue(ERGPassFlags::Copy) != EQueueType::Copy) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass queue flags mapped to the wrong queue"));
        return false;
    }
    return true;
}

bool ValidateRangeOverlapRules() {
    if (!RGBufferRange{.offset = 0, .size = 0}.Overlaps(RGBufferRange{.offset = 64, .size = 16})) {
        LOG_ERROR(MOER_TEXT("Whole-buffer RenderGraph range did not overlap a partial range"));
        return false;
    }
    if (RGBufferRange{.offset = 0, .size = 16}.Overlaps(RGBufferRange{.offset = 16, .size = 16})) {
        LOG_ERROR(MOER_TEXT("Adjacent RenderGraph buffer ranges overlapped"));
        return false;
    }
    if (!RGBufferRange{.offset = 8, .size = 16}.Overlaps(RGBufferRange{.offset = 16, .size = 8})) {
        LOG_ERROR(MOER_TEXT("Intersecting RenderGraph buffer ranges did not overlap"));
        return false;
    }

    const RGTextureRange mip0{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1};
    const RGTextureRange mip1{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1};
    const RGTextureRange mip0_depth{.aspect = ETextureAspectFlags::DEPTH_SLICE, .mip_min = 0, .mip_count = 1};
    if (mip0.Overlaps(mip1)) {
        LOG_ERROR(MOER_TEXT("Different RenderGraph texture mip ranges overlapped"));
        return false;
    }
    if (mip0.Overlaps(mip0_depth)) {
        LOG_ERROR(MOER_TEXT("Different RenderGraph texture aspects overlapped"));
        return false;
    }
    return true;
}

RGTextureDesc MakeColorTextureDesc(Extent3D extent, ETextureUsageFlags usage, uint8_t num_mips = 1) {
    RGTextureDesc desc{
        ETextureDimension::TEX_2D,
        usage,
        PF_R8G8B8A8_UNORM,
        EClearAttachment::COLOR,
        extent,
        num_mips,
        1,
        1
    };
    desc.aspect_flags = ETextureAspectFlags::COLOR;
    return desc;
}

template<typename T>
std::span<byte> ToByteSpan(std::vector<T>& values) {
    return std::span<byte>(reinterpret_cast<byte*>(values.data()), values.size() * sizeof(T));
}

template<typename T, size_t Size>
std::span<byte> ToByteSpan(std::array<T, Size>& values) {
    return std::span<byte>(reinterpret_cast<byte*>(values.data()), values.size() * sizeof(T));
}

template<typename T>
Array<byte> MakeTypedUploadBytes(std::span<const T> source) {
    Array<byte> bytes(source.size_bytes());
    std::memcpy(bytes.data(), source.data(), source.size_bytes());
    return bytes;
}

std::vector<uint8_t> MakeRgba8Pattern(uint32_t width, uint32_t height, uint32_t seed) {
    std::vector<uint8_t> bytes(size_t(width) * size_t(height) * 4u, 0u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = size_t(y) * size_t(width) + x;
            bytes[pixel * 4u + 0u] = static_cast<uint8_t>((x * 19u + seed) & 0xffu);
            bytes[pixel * 4u + 1u] = static_cast<uint8_t>((y * 29u + seed * 3u) & 0xffu);
            bytes[pixel * 4u + 2u] = static_cast<uint8_t>(((x ^ y) * 13u + seed * 7u) & 0xffu);
            bytes[pixel * 4u + 3u] = 255u;
        }
    }
    return bytes;
}

bool ValidateBytes(std::span<const uint8_t> expected, std::span<const uint8_t> actual, StringView label) {
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            LOG_ERROR(
                MOER_TEXT("{} mismatch at byte={}, expected={}, got={}"),
                label,
                i,
                expected[i],
                actual[i]
            );
            return false;
        }
    }
    return true;
}

bool ValidateWords(std::span<const uint32_t> expected, std::span<const uint32_t> actual, StringView label) {
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            LOG_ERROR(
                MOER_TEXT("{} mismatch at index={}, expected={}, got={}"),
                label,
                i,
                expected[i],
                actual[i]
            );
            return false;
        }
    }
    return true;
}

float Frac(float value) {
    return value - std::floor(value);
}

bool ValidateFloat4Words(
    std::span<const float4>   expected,
    std::span<const uint32_t> actual_words,
    StringView                label,
    float                     epsilon = 1.0e-4f
) {
    if (actual_words.size() != expected.size() * 4u) {
        LOG_ERROR(
            MOER_TEXT("{} word count mismatch, expected={}, got={}"),
            label,
            expected.size() * 4u,
            actual_words.size()
        );
        return false;
    }

    for (size_t pixel = 0; pixel < expected.size(); ++pixel) {
        for (size_t channel = 0; channel < 4u; ++channel) {
            const float expected_value = expected[pixel][channel];
            const float actual_value = std::bit_cast<float>(actual_words[pixel * 4u + channel]);
            if (std::fabs(expected_value - actual_value) > epsilon) {
                LOG_ERROR(
                    MOER_TEXT("{} mismatch at pixel={}, channel={}, expected={}, got={}"),
                    label,
                    pixel,
                    channel,
                    expected_value,
                    actual_value
                );
                return false;
            }
        }
    }
    return true;
}

StringView VisualizeModeLabel(uint32_t mode) {
    switch (mode) {
        case EFC_POSITION:
            return MOER_TEXT("RenderGraphGBufferVisualize.Position");
        case EFC_NORMAL:
            return MOER_TEXT("RenderGraphGBufferVisualize.Normal");
        case EFC_VIEW_DEPTH:
            return MOER_TEXT("RenderGraphGBufferVisualize.ViewDepth");
        case EFC_DEPTH:
            return MOER_TEXT("RenderGraphGBufferVisualize.Depth");
        case EFC_MATERIAL:
            return MOER_TEXT("RenderGraphGBufferVisualize.Material");
        case EFC_MOTION:
            return MOER_TEXT("RenderGraphGBufferVisualize.Motion");
        default:
            return MOER_TEXT("RenderGraphGBufferVisualize.Unknown");
    }
}

StringView VisualizeParamsResourceName(uint32_t mode) {
    switch (mode) {
        case EFC_POSITION:
            return MOER_TEXT("RGGBufferVisualize.params.position");
        case EFC_NORMAL:
            return MOER_TEXT("RGGBufferVisualize.params.normal");
        case EFC_VIEW_DEPTH:
            return MOER_TEXT("RGGBufferVisualize.params.view_depth");
        case EFC_DEPTH:
            return MOER_TEXT("RGGBufferVisualize.params.depth");
        case EFC_MATERIAL:
            return MOER_TEXT("RGGBufferVisualize.params.material");
        case EFC_MOTION:
            return MOER_TEXT("RGGBufferVisualize.params.motion");
        default:
            return MOER_TEXT("RGGBufferVisualize.params.unknown");
    }
}

StringView VisualizeOutputResourceName(uint32_t mode) {
    switch (mode) {
        case EFC_POSITION:
            return MOER_TEXT("RGGBufferVisualize.output.position");
        case EFC_NORMAL:
            return MOER_TEXT("RGGBufferVisualize.output.normal");
        case EFC_VIEW_DEPTH:
            return MOER_TEXT("RGGBufferVisualize.output.view_depth");
        case EFC_DEPTH:
            return MOER_TEXT("RGGBufferVisualize.output.depth");
        case EFC_MATERIAL:
            return MOER_TEXT("RGGBufferVisualize.output.material");
        case EFC_MOTION:
            return MOER_TEXT("RGGBufferVisualize.output.motion");
        default:
            return MOER_TEXT("RGGBufferVisualize.output.unknown");
    }
}

uint32_t FloatBits(float value) {
    return std::bit_cast<uint32_t>(value);
}

bool IsRayQuerySupported() {
    auto& vk_device = static_cast<VulkanDevice&>(*RenderDevice::Get().GetImpl());
    const auto& optional_extensions = vk_device.GetOptionalExtensions();
    return optional_extensions.m_has_khr_acceleration_structure && optional_extensions.m_has_khr_ray_query;
}

float RayQueryTriangleEdge(float2 a, float2 b, float2 p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

bool RayQueryPointInTriangle(float2 p, float2 a, float2 b, float2 c) {
    constexpr float epsilon = 1.0e-5f;
    const float ab = RayQueryTriangleEdge(a, b, p);
    const float bc = RayQueryTriangleEdge(b, c, p);
    const float ca = RayQueryTriangleEdge(c, a, p);
    return (ab >= -epsilon && bc >= -epsilon && ca >= -epsilon) ||
           (ab <= epsilon && bc <= epsilon && ca <= epsilon);
}

uint32_t ExpectedRayQueryTriangleValue(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    static constexpr uint32_t hit_base = 0x7a510000u;
    const float2 p(
        (static_cast<float>(x) + 0.5f) / static_cast<float>(width) * 2.0f - 1.0f,
        1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * 2.0f
    );
    const bool hit = RayQueryPointInTriangle(
        p,
        float2(-0.75f, -0.75f),
        float2(0.75f, -0.75f),
        float2(0.0f, 0.75f)
    );
    return hit ? hit_base | ((y & 0xffu) << 8u) | (x & 0xffu) : 0u;
}

RGTextureView WholeTextureView(RGTexture* texture) {
    const RGTextureDesc& desc = texture->Desc();
    return RGTextureView{
        .texture = texture,
        .range   = RGTextureRange{
            .aspect      = desc.aspect_flags == ETextureAspectFlags::NONE ? ETextureAspectFlags::COLOR : desc.aspect_flags,
            .mip_min     = 0,
            .mip_count   = desc.num_mips == 0 ? 1u : static_cast<uint32_t>(desc.num_mips),
            .array_min   = 0,
            .array_count = desc.array_size == 0 ? 1u : static_cast<uint32_t>(desc.array_size)
        }
    };
}

RGBuffer* ImportRaytracingGeometryBuffer(RenderGraph& graph, StringView name, const RaytracingGeometryRef& geometry) {
    assert(geometry && geometry->GetUnderlyingBuffer());
    return graph.ImportBuffer(name, BufferRef(geometry->GetUnderlyingBuffer()), EQueueType::Graphics);
}

RGBuffer* ImportRaytracingTlasBuffer(RenderGraph& graph, StringView name, const RaytracingTlasRef& tlas) {
    assert(tlas && tlas->GetUnderlyingBuffer());
    return graph.ImportBuffer(name, BufferRef(tlas->GetUnderlyingBuffer()), EQueueType::Graphics);
}

Array<byte> MakeUploadBytes(std::span<const uint8_t> source) {
    Array<byte> bytes(source.size());
    std::memcpy(bytes.data(), source.data(), source.size());
    return bytes;
}

} // namespace

int RunRenderGraphContractFoundationTest() {
    if (!ValidatePassDomainHelpers() || !ValidateRangeOverlapRules()) {
        return 1;
    }

    PooledTexturePool texture_pool{3};
    PooledBufferPool  buffer_pool{3};
    RGTransientResourceAllocator allocator{texture_pool, buffer_pool};
    RenderGraph       graph;

    RGTexture* texture = graph.CreateTexture(
        MOER_TEXT("rg_contract_texture"),
        MakeColorTextureDesc(
            Extent3D(16, 16, 1),
            ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED,
            2
        )
    );
    RGBufferDesc contract_buffer_desc{};
    contract_buffer_desc.size   = 256;
    contract_buffer_desc.stride = 1;
    contract_buffer_desc.usage  = EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC;
    RGBuffer* buffer = graph.CreateBuffer(MOER_TEXT("rg_contract_buffer"), contract_buffer_desc);

    auto* write_params    = graph.Alloc<RGExecutionParams>();
    write_params->texture = RGTextureView{
        .texture = texture,
        .range   = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
    };
    write_params->buffer = RGBufferView{.buffer = buffer, .range = RGBufferRange{.offset = 32, .size = 32}};

    auto* access_array_params = graph.Alloc<RGAccessArrayParams>();
    auto* serial_params       = graph.Alloc<RGSerialParams>();
    std::atomic_bool     resources_allocated{false};
    std::atomic_uint32_t write_execution_count{0};
    std::atomic_uint32_t serial_execution_count{0};

    access_array_params->textures       = graph.Alloc<RGTextureAccessArray>(2);
    access_array_params->buffers        = graph.Alloc<RGBufferAccessArray>(2);
    access_array_params->nested.texture = RGTextureView{
        .texture = texture,
        .range   = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
    };
    access_array_params->nested.buffer =
        RGBufferView{.buffer = buffer, .range = RGBufferRange{.offset = 48, .size = 8}};

    access_array_params->textures->AddAccess(
        RGTextureView{
            .texture = texture,
            .range   = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 0, .mip_count = 1}
        },
        ETextureState::SHADER_RESOURCE
    );
    access_array_params->textures->AddAccess(
        RGTextureView{
            .texture = texture,
            .range   = RGTextureRange{.aspect = ETextureAspectFlags::COLOR, .mip_min = 1, .mip_count = 1}
        },
        ETextureState::SHADER_RESOURCE
    );
    access_array_params->buffers->AddAccess(
        RGBufferView{.buffer = buffer, .range = RGBufferRange{.offset = 48, .size = 8}},
        EBufferState::SHADER_RESOURCE
    );
    access_array_params->buffers->AddAccess(
        RGBufferView{.buffer = buffer, .range = RGBufferRange{.offset = 96, .size = 16}},
        EBufferState::SHADER_RESOURCE
    );

    graph.AddSetupPass(
        MOER_TEXT("PrepareContractFoundation"),
        [write_params](RGSetupContext& setup) {
            (void)setup;
            write_params->prepared_value = 42;
        }
    );
    graph.AddPass(
        MOER_TEXT("WriteContractResources"),
        write_params,
        ERGPassFlags::Graphics,
        [write_params,
         texture,
         buffer,
         &resources_allocated,
         &write_execution_count](RHICommandList&, RGContext) {
            resources_allocated = texture->IsAllocated() && buffer->IsAllocated();
            if (write_params->prepared_value == 42) {
                ++write_execution_count;
            }
        }
    );

    graph.AddPass(
        MOER_TEXT("SerialContractFence"),
        serial_params,
        ERGPassFlags::Graphics | ERGPassFlags::Serial,
        [&serial_execution_count](RHICommandList&, RGContext) {
            ++serial_execution_count;
        }
    );

    graph.AddPass(
        MOER_TEXT("ReadAccessArrays"),
        access_array_params,
        ERGPassFlags::Compute,
        [](RHICommandList&, RGContext) {}
    );

    if (graph.GetPasses()[0].texture_accesses.size() != 1 ||
        graph.GetPasses()[0].buffer_accesses.size() != 1 ||
        graph.GetPasses()[2].texture_accesses.size() != 3 ||
        graph.GetPasses()[2].buffer_accesses.size() != 3) {
        LOG_ERROR(MOER_TEXT("RenderGraph pass accesses were not finalized when AddPass returned"));
        return 1;
    }

    graph.ExportTexture(texture, ETextureState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(buffer, EBufferState::SHADER_RESOURCE, EQueueType::Compute);

    graph.Dispatch(allocator);

    const RGCompiledPlan& plan = graph.GetCompiledPlan();
    if (plan.hazard_edges.size() != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation expected 2 hazards, got {}"), plan.hazard_edges.size());
        return 1;
    }
    if (plan.execution_batches.size() != 3 || plan.execution_batches[0].queue != EQueueType::Graphics ||
        plan.execution_batches[1].queue != EQueueType::Graphics ||
        plan.execution_batches[2].queue != EQueueType::Compute ||
        plan.execution_batches[0].first_pass != 0 || plan.execution_batches[0].pass_count != 1 ||
        plan.execution_batches[1].first_pass != 1 || plan.execution_batches[1].pass_count != 1 ||
        plan.execution_batches[2].first_pass != 2 || plan.execution_batches[2].pass_count != 1) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong execution batches"));
        return 1;
    }
    if (!HasHazard(plan, 0, 2, texture, ERGResourceKind::Texture) ||
        !HasHazard(plan, 0, 2, buffer, ERGResourceKind::Buffer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong hazard edges"));
        return 1;
    }
    const RGCompiledHazardEdge* texture_edge = FindHazard(plan, 0, 2, texture, ERGResourceKind::Texture);
    if (!texture_edge || !RGCompiledHazardHasFlag(*texture_edge, ERGCompiledHazardFlag::AccessConflict) ||
        !RGCompiledHazardHasFlag(*texture_edge, ERGCompiledHazardFlag::OwnerTransfer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation did not mark the texture owner-transfer hazard"));
        return 1;
    }
    const RGCompiledHazardEdge* buffer_edge = FindHazard(plan, 0, 2, buffer, ERGResourceKind::Buffer);
    if (!buffer_edge || !RGCompiledHazardHasFlag(*buffer_edge, ERGCompiledHazardFlag::AccessConflict) ||
        !RGCompiledHazardHasFlag(*buffer_edge, ERGCompiledHazardFlag::OwnerTransfer)) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation did not mark the buffer owner-transfer hazard"));
        return 1;
    }
    const RGResource& texture_resource = *texture;
    const RGResource& buffer_resource  = *buffer;
    if (texture_resource.compile.first_pass != 0 || texture_resource.compile.last_pass != 2 ||
        texture_resource.compile.access_write || buffer_resource.compile.first_pass != 0 ||
        buffer_resource.compile.last_pass != 2 || buffer_resource.compile.access_write) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong resource lifetime metadata"));
        return 1;
    }
    const RGPass& write_pass = graph.GetPasses()[0];
    const RGPass& read_pass  = graph.GetPasses()[2];
    if (read_pass.compile.last_pass != 0 ||
        read_pass.compile.last_pass_by_queue[static_cast<size_t>(EQueueType::Graphics)] != 0 ||
        write_pass.compile.next_pass_by_queue[static_cast<size_t>(EQueueType::Compute)] != 2) {
        LOG_ERROR(MOER_TEXT("RenderGraph foundation compiled wrong pass dependency metadata"));
        return 1;
    }
    if (texture->state_ranges.size() != 2 || buffer->state_ranges.size() != 7) {
        LOG_ERROR(MOER_TEXT("RenderGraph did not build the expected finest resource state ranges"));
        return 1;
    }
    if (write_params->prepared_value != 42) {
        LOG_ERROR(MOER_TEXT("RenderGraph setup lambda did not run before dispatch"));
        return 1;
    }
    RHIExecutor::Get().Sync();
    if (!resources_allocated) {
        LOG_ERROR(MOER_TEXT("RenderGraph transient resources were not allocated before execution"));
        return 1;
    }
    if (write_execution_count != 1 || serial_execution_count != 1) {
        LOG_ERROR(MOER_TEXT("RenderGraph setup or execution lambda order is invalid"));
        return 1;
    }
    if (texture_pool.LiveCount() == 0 || buffer_pool.LiveCount() == 0) {
        LOG_ERROR(MOER_TEXT("RenderGraph resource pools did not retain released resources"));
        return 1;
    }

    PooledTextureRef registered_texture = texture_pool.Allocate(
        MOER_TEXT("rg_registered_texture"),
        MakeColorTextureDesc(Extent3D(8, 8, 1), ETextureUsageFlags::SAMPLED)
    );
    RGBufferDesc registered_buffer_desc{};
    registered_buffer_desc.size   = 64;
    registered_buffer_desc.stride = 1;
    registered_buffer_desc.usage  = EBufferUsageFlags::UNORDERED_ACCESS;
    PooledBufferRef registered_buffer = buffer_pool.Allocate(MOER_TEXT("rg_registered_buffer"), registered_buffer_desc);

    RenderGraph register_graph;
    RGTexture*  registered_texture_handle = register_graph.RegisterTexture(
        MOER_TEXT("rg_registered_texture"), registered_texture, EQueueType::Graphics
    );
    RGBuffer* registered_buffer_handle = register_graph.RegisterBuffer(
        MOER_TEXT("rg_registered_buffer"), registered_buffer, EQueueType::Compute
    );
    if (registered_texture_handle->Pooled() != registered_texture ||
        registered_buffer_handle->Pooled() != registered_buffer) {
        LOG_ERROR(MOER_TEXT("RenderGraph did not register pooled resources directly"));
        return 1;
    }

    registered_texture.reset();
    registered_buffer.reset();
    register_graph.Reset();
    RHIExecutor::Get().Sync();
    for (uint32_t frame = 0; frame < 4; ++frame) {
        texture_pool.Tick();
        buffer_pool.Tick();
    }
    if (texture_pool.LiveCount() != 0 || buffer_pool.LiveCount() != 0) {
        LOG_ERROR(MOER_TEXT("RenderGraph resource pools did not destroy multi-frame idle resources"));
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph contract foundation test passed, hazards={}"), plan.hazard_edges.size());
    return 0;
}

int RunRenderGraphTextureCopyReadbackTest() {
    static constexpr uint32_t texture_size = 8;

    auto& device = RenderDevice::Get();
    auto source_texture = device.CreateTexture(
        MOER_TEXT("rg_texture_readback_source"),
        Extent2D(texture_size, texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
    );
    auto first_copy_texture = device.CreateTexture(
        MOER_TEXT("rg_texture_readback_first_copy"),
        Extent2D(texture_size, texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
    );
    auto second_copy_texture = device.CreateTexture(
        MOER_TEXT("rg_texture_readback_second_copy"),
        Extent2D(texture_size, texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
    );

    const std::vector<uint8_t> expected = MakeRgba8Pattern(texture_size, texture_size, 23u);

    RenderGraph graph;
    RGTexture* rg_source = graph.ImportTexture(MOER_TEXT("RGTest.source"), source_texture, EQueueType::Graphics);
    RGTexture* rg_first_copy = graph.ImportTexture(
        MOER_TEXT("RGTest.first_copy"),
        first_copy_texture,
        EQueueType::Graphics
    );
    RGTexture* rg_second_copy = graph.ImportTexture(
        MOER_TEXT("RGTest.second_copy"),
        second_copy_texture,
        EQueueType::Graphics
    );

    auto* upload_params    = graph.Alloc<RGTextureUploadParams>();
    upload_params->target  = WholeTextureView(rg_source);
    graph.AddPass(
        MOER_TEXT("RGTest.UploadSourceTexture"),
        upload_params,
        ERGPassFlags::Graphics,
        [rg_source, expected](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(MakeUploadBytes(expected), rg_source->RHI()->GetView(), MOER_TEXT("RGTestUploadSource"));
        }
    );

    auto* first_copy_params = graph.Alloc<RGTextureCopyParams>();
    first_copy_params->src  = WholeTextureView(rg_source);
    first_copy_params->dst  = WholeTextureView(rg_first_copy);
    auto* first_copy_accesses = graph.Alloc<RGTextureAccessArray>(2);
    first_copy_accesses->AddAccess(first_copy_params->src, ETextureState::TRANSFER_SRC);
    first_copy_accesses->AddAccess(first_copy_params->dst, ETextureState::TRANSFER_DST);
    first_copy_params->accesses = first_copy_accesses;
    graph.AddPass(
        MOER_TEXT("RGTest.CopySourceToFirst"),
        first_copy_params,
        ERGPassFlags::Graphics,
        [rg_source, rg_first_copy](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(
                rg_source->RHI()->GetView(),
                rg_first_copy->RHI()->GetView(),
                MOER_TEXT("RGTestSourceToFirst")
            );
        }
    );

    auto* second_copy_params = graph.Alloc<RGTextureCopyParams>();
    second_copy_params->src  = WholeTextureView(rg_first_copy);
    second_copy_params->dst  = WholeTextureView(rg_second_copy);
    auto* second_copy_accesses = graph.Alloc<RGTextureAccessArray>(2);
    second_copy_accesses->AddAccess(second_copy_params->src, ETextureState::TRANSFER_SRC);
    second_copy_accesses->AddAccess(second_copy_params->dst, ETextureState::TRANSFER_DST);
    second_copy_params->accesses = second_copy_accesses;
    graph.AddPass(
        MOER_TEXT("RGTest.CopyFirstToSecond"),
        second_copy_params,
        ERGPassFlags::Graphics,
        [rg_first_copy, rg_second_copy](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(
                rg_first_copy->RHI()->GetView(),
                rg_second_copy->RHI()->GetView(),
                MOER_TEXT("RGTestFirstToSecond")
            );
        }
    );

    graph.ExportTexture(rg_source, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_first_copy, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_second_copy, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.Dispatch();

    const RGCompiledPlan& plan = graph.GetCompiledPlan();
    if (!HasHazard(plan, 0, 1, rg_source, ERGResourceKind::Texture) ||
        !HasHazard(plan, 1, 2, rg_first_copy, ERGResourceKind::Texture)) {
        LOG_ERROR(MOER_TEXT("RenderGraph texture readback test did not compile expected copy hazards"));
        return 1;
    }

    std::vector<uint8_t> source_readback(expected.size(), 0u);
    std::vector<uint8_t> first_copy_readback(expected.size(), 0u);
    std::vector<uint8_t> second_copy_readback(expected.size(), 0u);

    CommandList readback_cmd(EQueueType::Graphics);
    readback_cmd.Barriers(
        {
            BarrierCreateInfo::Transition(
                source_texture->GetView(),
                ETextureState::TRANSFER_SRC,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            ),
            BarrierCreateInfo::Transition(
                first_copy_texture->GetView(),
                ETextureState::TRANSFER_SRC,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            ),
            BarrierCreateInfo::Transition(
                second_copy_texture->GetView(),
                ETextureState::TRANSFER_SRC,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            )
        },
        EQueueType::Graphics,
        EQueueType::Graphics
    );
    SyncPointRef source_event = readback_cmd.ReadbackCopy(
        source_texture->GetView(),
        ToByteSpan(source_readback),
        MOER_TEXT("RGTestReadbackSource")
    );
    SyncPointRef first_copy_event = readback_cmd.ReadbackCopy(
        first_copy_texture->GetView(),
        ToByteSpan(first_copy_readback),
        MOER_TEXT("RGTestReadbackFirstCopy")
    );
    SyncPointRef second_copy_event = readback_cmd.ReadbackCopy(
        second_copy_texture->GetView(),
        ToByteSpan(second_copy_readback),
        MOER_TEXT("RGTestReadbackSecondCopy")
    );

    Array<CommandList> readback_lists{};
    readback_lists.emplace_back(std::move(readback_cmd));
    RHIExecutor::Get().Submit(std::move(readback_lists), ERHIExecSubmitFlags::FlushGPU);
    if (source_event) {
        source_event->WaitHost();
    }
    if (first_copy_event) {
        first_copy_event->WaitHost();
    }
    if (second_copy_event) {
        second_copy_event->WaitHost();
    }

    if (!ValidateBytes(expected, source_readback, MOER_TEXT("RGTest.UploadSourceTexture"))) {
        return 1;
    }
    if (!ValidateBytes(expected, first_copy_readback, MOER_TEXT("RGTest.CopySourceToFirst"))) {
        return 1;
    }
    if (!ValidateBytes(expected, second_copy_readback, MOER_TEXT("RGTest.CopyFirstToSecond"))) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph texture copy/readback test passed"));
    return 0;
}

int RunRenderGraphDispatchBindlessReadbackTest() {
    static constexpr uint32_t element_count = 128;
    static constexpr uint32_t xor_mask      = 0x4b1d5a77u;

    auto& device = RenderDevice::Get();
    auto bindless_array = device.CreateBindlessArray();
    auto source_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_dispatch_bindless_source"),
        element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto output_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_dispatch_bindless_output"),
        element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    const uint32_t source_handle = bindless_array->AllocateBuffer(source_buffer->GetView());
    auto pipeline_object = ShaderManager::Get().Compute<RGBindlessDispatchPipeline>(
        "tests/RGDispatchBindless.comp.hlsl"
    );
    auto pipeline = MakeShared<RGBindlessDispatchPipeline>(std::move(pipeline_object));

    std::vector<uint32_t> source_values(element_count);
    std::vector<uint32_t> expected_values(element_count);
    std::vector<uint32_t> readback_values(element_count, 0u);
    for (uint32_t i = 0; i < element_count; ++i) {
        source_values[i]   = 0x220000u + i * 37u + 19u;
        expected_values[i] = source_values[i] ^ xor_mask;
    }

    RenderGraph graph;
    RGBuffer* rg_source = graph.ImportBuffer(MOER_TEXT("RGDispatchBindless.source"), source_buffer, EQueueType::Graphics);
    RGBuffer* rg_output = graph.ImportBuffer(MOER_TEXT("RGDispatchBindless.output"), output_buffer, EQueueType::Graphics);

    auto* upload_source_params   = graph.Alloc<RGBufferUploadParams>();
    upload_source_params->target = RGBufferView{.buffer = rg_source};
    graph.AddPass(
        MOER_TEXT("RGDispatchBindless.UploadSource"),
        upload_source_params,
        ERGPassFlags::Graphics,
        [rg_source, source_values](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<uint32_t>(std::span<const uint32_t>(source_values.data(), source_values.size())),
                rg_source->RHI()->GetView(),
                MOER_TEXT("RGDispatchBindlessUploadSource")
            );
        }
    );

    auto* clear_output_params   = graph.Alloc<RGBufferUploadParams>();
    clear_output_params->target = RGBufferView{.buffer = rg_output};
    graph.AddPass(
        MOER_TEXT("RGDispatchBindless.ClearOutput"),
        clear_output_params,
        ERGPassFlags::Graphics,
        [rg_output](RHICommandList& cmd_list, RGContext) {
            cmd_list.ClearResource(rg_output->RHI()->GetView(), 0u);
        }
    );

    auto* update_params = graph.Alloc<RGBindlessUpdateParams>();
    graph.AddPass(
        MOER_TEXT("RGDispatchBindless.UpdateBindless"),
        update_params,
        ERGPassFlags::Graphics,
        [bindless_array](RHICommandList& cmd_list, RGContext) {
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                WriteBindlessArray{bindless_array, EBufferState::UNORDERED_ACCESS}
            );
            cmd_list.UpdateBindlessArray(bindless_array);
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                ReadBindlessArray{bindless_array, EBufferState::SHADER_RESOURCE}
            );
        }
    );

    auto* dispatch_params    = graph.Alloc<RGBufferDispatchParams>();
    dispatch_params->source  = RGBufferView{.buffer = rg_source};
    dispatch_params->output  = RGBufferView{.buffer = rg_output};
    graph.AddPass(
        MOER_TEXT("RGDispatchBindless.Dispatch"),
        dispatch_params,
        ERGPassFlags::Graphics,
        [rg_output, bindless_array, pipeline, source_handle](RHICommandList& cmd_list, RGContext) mutable {
            cmd_list
                .Compute(
                    *pipeline,
                    RGBindlessDispatchArgs{
                        .src_handle    = source_handle,
                        .xor_mask      = xor_mask,
                        .element_count = element_count,
                        .pad           = 0u
                    },
                    rg_output->RHI()->GetView(),
                    bindless_array
                )
                .Dispatch((element_count + 63u) / 64u, MOER_TEXT("RGDispatchBindlessDispatch"));
        }
    );

    auto* readback_state_params   = graph.Alloc<RGBufferReadbackStateParams>();
    readback_state_params->source = RGBufferView{.buffer = rg_output};
    graph.AddPass(
        MOER_TEXT("RGDispatchBindless.PrepareReadback"),
        readback_state_params,
        ERGPassFlags::Graphics,
        [](RHICommandList&, RGContext) {}
    );

    graph.ExportBuffer(rg_source, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_output, EBufferState::TRANSFER_SRC, EQueueType::Graphics);
    graph.Dispatch();

    CommandList readback_cmd(EQueueType::Graphics);
    SyncPointRef readback_event = readback_cmd.ReadbackCopy(
        output_buffer->GetView(),
        ToByteSpan(readback_values),
        MOER_TEXT("RGDispatchBindlessReadback")
    );
    Array<CommandList> command_lists{};
    command_lists.emplace_back(std::move(readback_cmd));
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->WaitHost();
    }

    if (!ValidateWords(expected_values, readback_values, MOER_TEXT("RGDispatchBindless"))) {
        return 1;
    }

    LOG_INFO(MOER_TEXT("RenderGraph dispatch/bindless readback test passed, handle={}"), source_handle);
    return 0;
}

int RunRenderGraphRayQueryTriangleHitTest() {
    if (!IsRayQuerySupported()) {
        LOG_INFO(MOER_TEXT("[TESTCASE][SKIP] RenderGraphRayQueryTriangleHit :: ray query is unavailable"));
        return 0;
    }

    static constexpr uint32_t viewport_width = 10;
    static constexpr uint32_t viewport_height = 10;
    static constexpr uint32_t pixel_count = viewport_width * viewport_height;

    auto& device = RenderDevice::Get();
    auto vertex_buffer = device.CreateBuffer<float3>(
        MOER_TEXT("rg_rayquery_vertices"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::VERTEX_BUFFER
    );
    auto index_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_rayquery_indices"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::INDEX_BUFFER
    );
    auto output_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_rayquery_output"),
        pixel_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );

    std::array<float3, 6> vertices{
        float3(-10.0f, -10.0f, 0.0f),
        float3(-9.0f, -10.0f, 0.0f),
        float3(-10.0f, -9.0f, 0.0f),
        float3(-0.75f, -0.75f, 0.0f),
        float3(0.75f, -0.75f, 0.0f),
        float3(0.0f, 0.75f, 0.0f),
    };
    std::array<uint32_t, 6> indices{0u, 1u, 2u, 0u, 1u, 2u};
    std::array<uint32_t, pixel_count> expected{};
    std::array<uint32_t, pixel_count> readback{};
    for (uint32_t y = 0; y < viewport_height; ++y) {
        for (uint32_t x = 0; x < viewport_width; ++x) {
            expected[y * viewport_width + x] = ExpectedRayQueryTriangleValue(x, y, viewport_width, viewport_height);
        }
    }

    RaytracingGeometryInfo geometry_info{};
    geometry_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
    geometry_info.vertex_format = PF_R32G32B32_SFLOAT;
    geometry_info.index_type    = EIndexElementType::IET_UINT32;

    RaytracingSegment segment{};
    segment.vertex_offset    = 0;
    segment.index_offset     = 0;
    segment.first_vertex     = 3;
    segment.vertex_count     = 3;
    segment.vertex_stride    = sizeof(float3);
    segment.first_primitive  = 1;
    segment.primitive_count  = 1;
    segment.vertex_buffer    = vertex_buffer;
    segment.index_buffer     = index_buffer;
    segment.type             = RTGT_TRIANGLES;
    segment.flags            = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    segment.b_force_opaque   = false;
    segment.b_cull_back_face = false;
    segment.b_flip_face      = false;
    geometry_info.segments.emplace_back(segment);

    RaytracingGeometryRef geometry = device.CreateRaytracingGeometry(geometry_info);
    RaytracingSceneRef scene = device.CreateRaytracingScene();
    scene->RegisterGeometry(geometry);
    RaytracingInstance& instance = scene->AddInstance();
    instance.geom = geometry;
    instance.transform = Matrix3x4f(
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f
    );
    instance.custom_index = 0;
    instance.visible_mask = RTVM_ALL;
    scene->MarkModified(instance.instance_id);

    auto pipeline_object = ShaderManager::Get().Compute<RGRayQueryHitPipeline>(
        "tests/RGRayQueryHit.comp.hlsl"
    );
    auto pipeline = MakeShared<RGRayQueryHitPipeline>(std::move(pipeline_object));

    static constexpr uint32_t test_frame_count = 2;
    for (uint32_t frame_index = 0; frame_index < test_frame_count; ++frame_index) {
        readback.fill(0u);
        scene->MarkModified(instance.instance_id);

        RenderGraph graph;
        RGBuffer* rg_vertices = graph.ImportBuffer(MOER_TEXT("RGRayQuery.vertices"), vertex_buffer, EQueueType::Graphics);
        RGBuffer* rg_indices = graph.ImportBuffer(MOER_TEXT("RGRayQuery.indices"), index_buffer, EQueueType::Graphics);
        RGBuffer* rg_output = graph.ImportBuffer(MOER_TEXT("RGRayQuery.output"), output_buffer, EQueueType::Graphics);
        RGBuffer* rg_blas = ImportRaytracingGeometryBuffer(graph, MOER_TEXT("RGRayQuery.blas_buffer"), geometry);

        auto* upload_params = graph.Alloc<RGRayGeometryUploadParams>();
        upload_params->vertices = RGBufferView{.buffer = rg_vertices};
        upload_params->indices = RGBufferView{.buffer = rg_indices};
        upload_params->output = RGBufferView{.buffer = rg_output};
        graph.AddPass(
            MOER_TEXT("RGRayQuery.UploadGeometry"),
            upload_params,
            ERGPassFlags::Graphics,
            [rg_vertices, rg_indices, rg_output, vertices, indices](RHICommandList& cmd_list, RGContext) {
                cmd_list.CopyFrom(
                    MakeTypedUploadBytes<float3>(std::span<const float3>(vertices.data(), vertices.size())),
                    rg_vertices->RHI()->GetView(),
                    MOER_TEXT("RGRayQueryUploadVertices")
                );
                cmd_list.CopyFrom(
                    MakeTypedUploadBytes<uint32_t>(std::span<const uint32_t>(indices.data(), indices.size())),
                    rg_indices->RHI()->GetView(),
                    MOER_TEXT("RGRayQueryUploadIndices")
                );
                cmd_list.ClearResource(rg_output->RHI()->GetView(), 0u);
            }
        );

        auto* build_blas_params = graph.Alloc<RGRayBuildBlasParams>();
        build_blas_params->vertices = RGBufferView{.buffer = rg_vertices};
        build_blas_params->indices = RGBufferView{.buffer = rg_indices};
        build_blas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
        graph.AddPass(
            MOER_TEXT("RGRayQuery.BuildBLAS"),
            build_blas_params,
            ERGPassFlags::Graphics,
            [geometry](RHICommandList& cmd_list, RGContext) {
                cmd_list.BuildAccelerationStructures({AccelerationStructureBuildParam{
                    .geometry = geometry,
                    .mode     = ERaytracingBuildMode::BUILD,
                }});
            }
        );

        auto* prepared_tlas_update = graph.Alloc<RGRayPreparedTlasUpdate>();
        prepared_tlas_update->command = scene->UpdateScene();
        if (!prepared_tlas_update->command) {
            LOG_ERROR(MOER_TEXT("RenderGraph ray query hit test failed to prepare TLAS update command"));
            return 1;
        }

        RaytracingTlasRef tlas = scene->GetTlas();
        if (!tlas) {
            LOG_ERROR(MOER_TEXT("RenderGraph ray query hit test prepared a null TLAS"));
            return 1;
        }
        RGBuffer* rg_tlas = ImportRaytracingTlasBuffer(graph, MOER_TEXT("RGRayQuery.tlas_buffer"), tlas);

        auto* tlas_params = graph.Alloc<RGRayTlasParams>();
        tlas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
        tlas_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
        graph.AddPass(
            MOER_TEXT("RGRayQuery.BuildTLAS"),
            tlas_params,
            ERGPassFlags::Graphics,
            [prepared_tlas_update](RHICommandList& cmd_list, RGContext) {
                cmd_list.UpdateRaytracingScene(std::move(prepared_tlas_update->command));
            }
        );

        auto* trace_params = graph.Alloc<RGRayTraceParams>();
        trace_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
        trace_params->output = RGBufferView{.buffer = rg_output};
        graph.AddPass(
            MOER_TEXT("RGRayQuery.TraceInline"),
            trace_params,
            ERGPassFlags::Graphics | ERGPassFlags::ComputeShader,
            [rg_output, tlas, pipeline](RHICommandList& cmd_list, RGContext) mutable {
                cmd_list.Compute(*pipeline, tlas, rg_output->RHI()->GetView())
                    .Dispatch(uint3(viewport_width, viewport_height, 1u), MOER_TEXT("RGRayQueryTraceInline"));
            }
        );

        auto* readback_state_params = graph.Alloc<RGBufferReadbackStateParams>();
        readback_state_params->source = RGBufferView{.buffer = rg_output};
        graph.AddPass(
            MOER_TEXT("RGRayQuery.PrepareReadback"),
            readback_state_params,
            ERGPassFlags::Graphics,
            [](RHICommandList&, RGContext) {}
        );

        graph.ExportBuffer(rg_vertices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
        graph.ExportBuffer(rg_indices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
        graph.ExportBuffer(rg_output, EBufferState::TRANSFER_SRC, EQueueType::Graphics);
        graph.Dispatch();

        CommandList readback_cmd(EQueueType::Graphics);
        SyncPointRef readback_event = readback_cmd.ReadbackCopy(
            output_buffer->GetView(),
            ToByteSpan(readback),
            MOER_TEXT("RGRayQueryReadback")
        );
        Array<CommandList> command_lists{};
        command_lists.emplace_back(std::move(readback_cmd));
        RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->WaitHost();
        }

        if (!ValidateWords(expected, readback, MOER_TEXT("RenderGraphRayQueryTriangleHit"))) {
            return 1;
        }

        scene->AdvanceFrame();
    }

    LOG_INFO(MOER_TEXT("RenderGraph ray query triangle hit test passed, viewport={}x{}"), viewport_width, viewport_height);
    return 0;
}

int RunRenderGraphRayQuerySceneBindlessReadbackTest() {
    if (!IsRayQuerySupported()) {
        LOG_INFO(MOER_TEXT("[TESTCASE][SKIP] RenderGraphRayQuerySceneBindlessReadback :: ray query is unavailable"));
        return 0;
    }

    static constexpr uint32_t viewport_width = 10;
    static constexpr uint32_t viewport_height = 10;
    static constexpr uint32_t pixel_count = viewport_width * viewport_height;
    static constexpr uint32_t words_per_pixel = 16;
    static constexpr uint32_t word_count = pixel_count * words_per_pixel;
    static constexpr uint32_t expected_instance_index = 5;
    static constexpr uint32_t expected_primitive_index = 2;
    static constexpr uint32_t expected_material_index = 1;

    auto& device = RenderDevice::Get();
    auto bindless_array = device.CreateBindlessArray();
    auto instance_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_ray_scene_instances"),
        sizeof(GInstance) * 6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto primitive_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_ray_scene_primitives"),
        sizeof(GPrimitive) * 3,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto material_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_ray_scene_materials"),
        sizeof(GMaterial) * 2,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto position_buffer = device.CreateBuffer<float3>(
        MOER_TEXT("rg_ray_scene_positions"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::VERTEX_BUFFER
    );
    auto texcoord_buffer = device.CreateBuffer<float2>(
        MOER_TEXT("rg_ray_scene_texcoords"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto index_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_ray_scene_indices"),
        9,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::INDEX_BUFFER
    );
    auto output_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_ray_scene_bindless_output"),
        word_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );

    GBufferPassParams bindless_params{};
    bindless_params.instance_buf_hdl = bindless_array->AllocateBuffer(instance_buffer->GetView());
    bindless_params.primitive_buf_hdl = bindless_array->AllocateBuffer(primitive_buffer->GetView());
    bindless_params.material_buf_hdl = bindless_array->AllocateBuffer(material_buffer->GetView());
    bindless_params.position_buf_hdl = bindless_array->AllocateBuffer(position_buffer->GetView());
    bindless_params.texcoord0_buf_hdl = bindless_array->AllocateBuffer(texcoord_buffer->GetView());
    bindless_params.index_buf_hdl = bindless_array->AllocateBuffer(index_buffer->GetView());

    std::array<GInstance, 6> instances{};
    instances[expected_instance_index].world_transform = Matrix4x4f::Identity();
    instances[expected_instance_index].primitive_id = expected_primitive_index;

    std::array<GPrimitive, 3> primitives{};
    primitives[expected_primitive_index].material_idx = expected_material_index;
    primitives[expected_primitive_index].attribute_mask =
        GPrimitiveEAttributeMask::Position | GPrimitiveEAttributeMask::Texcoord0;
    primitives[expected_primitive_index].position_start_idx = 3;
    primitives[expected_primitive_index].texcoord0_start_idx = 3;
    primitives[expected_primitive_index].index_start_idx = 3;

    std::array<GMaterial, 2> materials{};
    materials[expected_material_index].albedo_factor = float4(0.25f, 0.5f, 0.75f, 1.0f);
    materials[expected_material_index].roughness_factor = 0.625f;
    materials[expected_material_index].metallic_factor = 0.125f;
    materials[expected_material_index].alpha_cutoff = 0.5f;

    std::array<float3, 6> positions{
        float3(-10.0f, -10.0f, 0.0f),
        float3(-9.0f, -10.0f, 0.0f),
        float3(-10.0f, -9.0f, 0.0f),
        float3(-0.75f, -0.75f, 0.0f),
        float3(0.75f, -0.75f, 0.0f),
        float3(0.0f, 0.75f, 0.0f),
    };
    std::array<float2, 6> texcoords{
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.25f, 0.75f),
        float2(0.75f, 0.75f),
        float2(0.5f, 0.25f),
    };
    std::array<uint32_t, 9> indices{0u, 1u, 2u, 0u, 1u, 2u, 2u, 1u, 0u};

    std::array<uint32_t, word_count> expected{};
    std::array<uint32_t, word_count> readback{};
    for (uint32_t y = 0; y < viewport_height; ++y) {
        for (uint32_t x = 0; x < viewport_width; ++x) {
            if (ExpectedRayQueryTriangleValue(x, y, viewport_width, viewport_height) == 0u) {
                continue;
            }
            const uint32_t base = (y * viewport_width + x) * words_per_pixel;
            expected[base + 0] = 0x5cee0000u | ((y & 0xffu) << 8u) | (x & 0xffu);
            expected[base + 1] = expected_instance_index;
            expected[base + 2] = 0u;
            expected[base + 3] = expected_primitive_index;
            expected[base + 4] = 3u;
            expected[base + 5] = 3u;
            expected[base + 6] = FloatBits(-0.75f);
            expected[base + 7] = FloatBits(-0.75f);
            expected[base + 8] = FloatBits(0.75f);
            expected[base + 9] = FloatBits(-0.75f);
            expected[base + 10] = FloatBits(0.625f);
            expected[base + 11] = FloatBits(0.25f);
            expected[base + 12] = FloatBits(1.0f);
            expected[base + 13] = FloatBits(0.0f);
            expected[base + 14] = 0u;
            expected[base + 15] = 0x600d0001u;
        }
    }

    RaytracingGeometryInfo geometry_info{};
    geometry_info.build_flags = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
    geometry_info.vertex_format = PF_R32G32B32_SFLOAT;
    geometry_info.index_type = EIndexElementType::IET_UINT32;

    RaytracingSegment segment{};
    segment.vertex_offset = 0;
    segment.index_offset = 0;
    segment.first_vertex = 3;
    segment.vertex_count = 3;
    segment.vertex_stride = sizeof(float3);
    segment.first_primitive = 1;
    segment.primitive_count = 1;
    segment.vertex_buffer = position_buffer;
    segment.index_buffer = index_buffer;
    segment.type = RTGT_TRIANGLES;
    segment.flags = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    segment.b_force_opaque = false;
    segment.b_cull_back_face = false;
    segment.b_flip_face = false;
    geometry_info.segments.emplace_back(segment);

    RaytracingGeometryRef geometry = device.CreateRaytracingGeometry(geometry_info);
    RaytracingSceneRef scene = device.CreateRaytracingScene();
    scene->RegisterGeometry(geometry);
    RaytracingInstance& instance = scene->AddInstance();
    instance.geom = geometry;
    instance.transform = Matrix3x4f(
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f
    );
    instance.custom_index = expected_instance_index;
    instance.visible_mask = RTVM_ALL;
    scene->MarkModified(instance.instance_id);

    auto pipeline_object = ShaderManager::Get().Compute<RGRaySceneBindlessPipeline>(
        "tests/RGRaySceneBindless.comp.hlsl"
    );
    auto pipeline = MakeShared<RGRaySceneBindlessPipeline>(std::move(pipeline_object));

    RenderGraph graph;
    RGBuffer* rg_instances = graph.ImportBuffer(MOER_TEXT("RGRayScene.instances"), instance_buffer, EQueueType::Graphics);
    RGBuffer* rg_primitives = graph.ImportBuffer(MOER_TEXT("RGRayScene.primitives"), primitive_buffer, EQueueType::Graphics);
    RGBuffer* rg_materials = graph.ImportBuffer(MOER_TEXT("RGRayScene.materials"), material_buffer, EQueueType::Graphics);
    RGBuffer* rg_positions = graph.ImportBuffer(MOER_TEXT("RGRayScene.positions"), position_buffer, EQueueType::Graphics);
    RGBuffer* rg_texcoords = graph.ImportBuffer(MOER_TEXT("RGRayScene.texcoords"), texcoord_buffer, EQueueType::Graphics);
    RGBuffer* rg_indices = graph.ImportBuffer(MOER_TEXT("RGRayScene.indices"), index_buffer, EQueueType::Graphics);
    RGBuffer* rg_output = graph.ImportBuffer(MOER_TEXT("RGRayScene.output"), output_buffer, EQueueType::Graphics);
    RGBuffer* rg_blas = ImportRaytracingGeometryBuffer(graph, MOER_TEXT("RGRayScene.blas_buffer"), geometry);

    auto* upload_params = graph.Alloc<RGRaySceneUploadParams>();
    auto* upload_accesses = graph.Alloc<RGBufferAccessArray>(7);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_instances}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_primitives}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_materials}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_positions}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_texcoords}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_indices}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_output}, EBufferState::TRANSFER_DST);
    upload_params->buffers = upload_accesses;
    graph.AddPass(
        MOER_TEXT("RGRayScene.UploadSceneBuffers"),
        upload_params,
        ERGPassFlags::Graphics,
        [rg_instances, rg_primitives, rg_materials, rg_positions, rg_texcoords, rg_indices, rg_output, instances, primitives, materials, positions, texcoords, indices](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GInstance>(std::span<const GInstance>(instances.data(), instances.size())),
                rg_instances->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadInstances")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GPrimitive>(std::span<const GPrimitive>(primitives.data(), primitives.size())),
                rg_primitives->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadPrimitives")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GMaterial>(std::span<const GMaterial>(materials.data(), materials.size())),
                rg_materials->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadMaterials")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<float3>(std::span<const float3>(positions.data(), positions.size())),
                rg_positions->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadPositions")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<float2>(std::span<const float2>(texcoords.data(), texcoords.size())),
                rg_texcoords->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadTexcoords")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<uint32_t>(std::span<const uint32_t>(indices.data(), indices.size())),
                rg_indices->RHI()->GetView(),
                MOER_TEXT("RGRaySceneUploadIndices")
            );
            cmd_list.ClearResource(rg_output->RHI()->GetView(), 0u);
        }
    );

    auto* build_blas_params = graph.Alloc<RGRayBuildBlasParams>();
    build_blas_params->vertices = RGBufferView{.buffer = rg_positions};
    build_blas_params->indices = RGBufferView{.buffer = rg_indices};
    build_blas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
    graph.AddPass(
        MOER_TEXT("RGRayScene.BuildBLAS"),
        build_blas_params,
        ERGPassFlags::Graphics,
        [geometry](RHICommandList& cmd_list, RGContext) {
            cmd_list.BuildAccelerationStructures({AccelerationStructureBuildParam{
                .geometry = geometry,
                .mode = ERaytracingBuildMode::BUILD,
            }});
        }
    );

    auto* prepared_tlas_update = graph.Alloc<RGRayPreparedTlasUpdate>();
    prepared_tlas_update->command = scene->UpdateScene();
    if (!prepared_tlas_update->command) {
        LOG_ERROR(MOER_TEXT("RenderGraph ray scene bindless test failed to prepare TLAS update command"));
        return 1;
    }

    RaytracingTlasRef tlas = scene->GetTlas();
    if (!tlas) {
        LOG_ERROR(MOER_TEXT("RenderGraph ray scene bindless test prepared a null TLAS"));
        return 1;
    }
    RGBuffer* rg_tlas = ImportRaytracingTlasBuffer(graph, MOER_TEXT("RGRayScene.tlas_buffer"), tlas);

    auto* tlas_params = graph.Alloc<RGRayTlasParams>();
    tlas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
    tlas_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
    graph.AddPass(
        MOER_TEXT("RGRayScene.BuildTLAS"),
        tlas_params,
        ERGPassFlags::Graphics,
        [prepared_tlas_update](RHICommandList& cmd_list, RGContext) {
            cmd_list.UpdateRaytracingScene(std::move(prepared_tlas_update->command));
        }
    );

    auto* update_params = graph.Alloc<RGBindlessUpdateParams>();
    graph.AddPass(
        MOER_TEXT("RGRayScene.UpdateBindless"),
        update_params,
        ERGPassFlags::Graphics,
        [bindless_array](RHICommandList& cmd_list, RGContext) {
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                WriteBindlessArray{bindless_array, EBufferState::UNORDERED_ACCESS}
            );
            cmd_list.UpdateBindlessArray(bindless_array);
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                ReadBindlessArray{bindless_array, EBufferState::SHADER_RESOURCE}
            );
        }
    );

    auto* trace_params = graph.Alloc<RGRaySceneTraceParams>();
    auto* scene_read_accesses = graph.Alloc<RGBufferAccessArray>(6);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_instances}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_primitives}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_materials}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_positions}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_texcoords}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_indices}, EBufferState::SHADER_RESOURCE);
    trace_params->scene_buffers = scene_read_accesses;
    trace_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
    trace_params->output = RGBufferView{.buffer = rg_output};
    graph.AddPass(
        MOER_TEXT("RGRayScene.TraceBindless"),
        trace_params,
        ERGPassFlags::Graphics | ERGPassFlags::ComputeShader,
        [rg_output, tlas, bindless_array, pipeline, bindless_params](RHICommandList& cmd_list, RGContext) mutable {
            cmd_list.Compute(*pipeline, bindless_params, tlas, rg_output->RHI()->GetView(), bindless_array)
                .Dispatch(uint3(viewport_width, viewport_height, 1u), MOER_TEXT("RGRaySceneTraceBindless"));
        }
    );

    auto* readback_state_params = graph.Alloc<RGBufferReadbackStateParams>();
    readback_state_params->source = RGBufferView{.buffer = rg_output};
    graph.AddPass(
        MOER_TEXT("RGRayScene.PrepareReadback"),
        readback_state_params,
        ERGPassFlags::Graphics,
        [](RHICommandList&, RGContext) {}
    );

    graph.ExportBuffer(rg_instances, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_primitives, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_materials, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_positions, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
    graph.ExportBuffer(rg_texcoords, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_indices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
    graph.ExportBuffer(rg_output, EBufferState::TRANSFER_SRC, EQueueType::Graphics);
    graph.Dispatch();

    CommandList readback_cmd(EQueueType::Graphics);
    SyncPointRef readback_event = readback_cmd.ReadbackCopy(
        output_buffer->GetView(),
        ToByteSpan(readback),
        MOER_TEXT("RGRaySceneBindlessReadback")
    );
    Array<CommandList> command_lists{};
    command_lists.emplace_back(std::move(readback_cmd));
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->WaitHost();
    }

    if (!ValidateWords(expected, readback, MOER_TEXT("RenderGraphRayQuerySceneBindlessReadback"))) {
        return 1;
    }

    scene->AdvanceFrame();

    LOG_INFO(MOER_TEXT("RenderGraph ray query scene bindless readback test passed, viewport={}x{}"), viewport_width, viewport_height);
    return 0;
}

int RunRenderGraphRayQueryGBufferOutputReadbackTest() {
    if (!IsRayQuerySupported()) {
        LOG_INFO(MOER_TEXT("[TESTCASE][SKIP] RenderGraphRayQueryGBufferOutputReadback :: ray query is unavailable"));
        return 0;
    }

    static constexpr uint32_t viewport_width = 10;
    static constexpr uint32_t viewport_height = 10;
    static constexpr uint32_t pixel_count = viewport_width * viewport_height;
    static constexpr uint32_t visualize_mode_count = 6;
    static constexpr std::array<uint32_t, visualize_mode_count> visualize_modes{
        EFC_POSITION,
        EFC_NORMAL,
        EFC_VIEW_DEPTH,
        EFC_DEPTH,
        EFC_MATERIAL,
        EFC_MOTION
    };
    static constexpr uint32_t expected_instance_index = 5;
    static constexpr uint32_t expected_primitive_index = 2;
    static constexpr uint32_t expected_material_index = 1;

    auto& device = RenderDevice::Get();
    auto bindless_array = device.CreateBindlessArray();
    auto constants_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_gbuffer_constants"),
        sizeof(GBufferConstants),
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::CONSTANT_BUFFER
    );
    auto instance_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_gbuffer_instances"),
        sizeof(GInstance) * 6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto primitive_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_gbuffer_primitives"),
        sizeof(GPrimitive) * 3,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto material_buffer = device.CreateBuffer<byte>(
        MOER_TEXT("rg_gbuffer_materials"),
        sizeof(GMaterial) * 2,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto position_buffer = device.CreateBuffer<float3>(
        MOER_TEXT("rg_gbuffer_positions"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::VERTEX_BUFFER
    );
    auto packed_normal_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_gbuffer_normals"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto texcoord_buffer = device.CreateBuffer<float2>(
        MOER_TEXT("rg_gbuffer_texcoords"),
        6,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto index_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_gbuffer_indices"),
        9,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::INDEX_BUFFER
    );

    const auto texture_usage = ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED |
                               ETextureUsageFlags::TRANSFER_SRC;
    auto view_depth = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_view_depth"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32_SFLOAT,
        texture_usage
    );
    auto diffuse_albedo = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_diffuse_albedo"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32_UINT,
        texture_usage
    );
    auto specular_roughness = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_specular_roughness"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32_UINT,
        texture_usage
    );
    auto normal = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_normal"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32_UINT,
        texture_usage
    );
    auto emission = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_emission"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R16G16B16A16_SFLOAT,
        texture_usage
    );
    auto motion = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_motion"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R16G16B16A16_SFLOAT,
        texture_usage
    );
    auto clip_depth = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_clip_depth"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32_SFLOAT,
        texture_usage
    );
    auto direct_lighting = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_visualize_direct_lighting"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32G32B32A32_SFLOAT,
        texture_usage
    );
    auto diffuse_lighting = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_visualize_diffuse_lighting"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32G32B32A32_SFLOAT,
        texture_usage
    );
    auto specular_lighting = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_visualize_specular_lighting"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R32G32B32A32_SFLOAT,
        texture_usage
    );
    auto normal_roughness = device.CreateTexture(
        MOER_TEXT("rg_gbuffer_visualize_normal_roughness"),
        Extent3D(viewport_width, viewport_height, 1),
        PF_R8G8B8A8_UNORM,
        texture_usage
    );

    std::array<BufferRef, visualize_mode_count> visualize_params_buffers{};
    std::array<TextureRef, visualize_mode_count> visualize_outputs{};
    for (uint32_t mode_index = 0; mode_index < visualize_mode_count; ++mode_index) {
        visualize_params_buffers[mode_index] = device.CreateBuffer<byte>(
            MOER_TEXT("rg_gbuffer_visualize_params"),
            sizeof(VisualizeParams),
            EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::CONSTANT_BUFFER
        );
        visualize_outputs[mode_index] = device.CreateTexture(
            MOER_TEXT("rg_gbuffer_visualize_output"),
            Extent3D(viewport_width, viewport_height, 1),
            PF_R32G32B32A32_SFLOAT,
            texture_usage
        );
    }

    GBufferPassParams bindless_params{};
    bindless_params.instance_buf_hdl = bindless_array->AllocateBuffer(instance_buffer->GetView());
    bindless_params.primitive_buf_hdl = bindless_array->AllocateBuffer(primitive_buffer->GetView());
    bindless_params.material_buf_hdl = bindless_array->AllocateBuffer(material_buffer->GetView());
    bindless_params.position_buf_hdl = bindless_array->AllocateBuffer(position_buffer->GetView());
    bindless_params.packed_normal_buf_hdl = bindless_array->AllocateBuffer(packed_normal_buffer->GetView());
    bindless_params.texcoord0_buf_hdl = bindless_array->AllocateBuffer(texcoord_buffer->GetView());
    bindless_params.index_buf_hdl = bindless_array->AllocateBuffer(index_buffer->GetView());

    GBufferConstants constants{};
    constants.main_view.view2world = Matrix4x4f::Identity();
    constants.main_view.world2view = Matrix4x4f::Identity();
    constants.main_view.world2clip = Matrix4x4f::Identity();
    constants.main_view.clip2view = Matrix4x4f::Identity();
    constants.main_view.view2clip = Matrix4x4f::Identity();
    constants.main_view.clip2world = Matrix4x4f::Identity();
    constants.main_view.near_far = float2(0.1f, 5000.0f);
    constants.main_view.rect = float2(static_cast<float>(viewport_width), static_cast<float>(viewport_height));
    constants.main_view.inv_rect = float2(1.0f / viewport_width, 1.0f / viewport_height);
    constants.main_view.clip2window_scale = float2(0.5f * viewport_width, -0.5f * viewport_height);
    constants.main_view.clip2window_bias = float2(0.5f * viewport_width, 0.5f * viewport_height);
    constants.main_view.window2clip_scale = float2(2.0f / viewport_width, -2.0f / viewport_height);
    constants.main_view.window2clip_bias = float2(-1.0f, 1.0f);
    constants.main_view.dir_or_pos = float4(0.0f, 0.0f, -1.0f, 1.0f);
    constants.prev_view = constants.main_view;

    std::array<GInstance, 6> instances{};
    instances[expected_instance_index].world_transform = Matrix4x4f::Identity();
    instances[expected_instance_index].primitive_id = expected_primitive_index;

    std::array<GPrimitive, 3> primitives{};
    primitives[expected_primitive_index].material_idx = expected_material_index;
    primitives[expected_primitive_index].attribute_mask = GPrimitiveEAttributeMask::Position |
                                                          GPrimitiveEAttributeMask::PackedNormal |
                                                          GPrimitiveEAttributeMask::Texcoord0;
    primitives[expected_primitive_index].position_start_idx = 3;
    primitives[expected_primitive_index].packed_normal_start_idx = 3;
    primitives[expected_primitive_index].texcoord0_start_idx = 3;
    primitives[expected_primitive_index].index_start_idx = 3;

    std::array<GMaterial, 2> materials{};
    materials[expected_material_index].albedo_factor = float4(0.25f, 0.5f, 0.75f, 1.0f);
    materials[expected_material_index].roughness_factor = 0.625f;
    materials[expected_material_index].metallic_factor = 0.0f;
    materials[expected_material_index].alpha_cutoff = 0.5f;

    std::array<float3, 6> positions{
        float3(-10.0f, -10.0f, 0.0f),
        float3(-9.0f, -10.0f, 0.0f),
        float3(-10.0f, -9.0f, 0.0f),
        float3(-4.0f, -4.0f, 0.0f),
        float3(0.0f, 4.0f, 0.0f),
        float3(4.0f, -4.0f, 0.0f),
    };
    const uint32_t packed_normal = Moer::Pack_Normal(float3(0.0f, 0.0f, 1.0f));
    std::array<uint32_t, 6> packed_normals{
        packed_normal,
        packed_normal,
        packed_normal,
        packed_normal,
        packed_normal,
        packed_normal,
    };
    std::array<float2, 6> texcoords{
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.5f, 0.0f),
        float2(1.0f, 1.0f),
    };
    std::array<uint32_t, 9> indices{0u, 1u, 2u, 0u, 1u, 2u, 0u, 1u, 2u};

    const uint32_t expected_diffuse = Moer::Pack_R11G11B10_UFLOAT(float3(0.25f, 0.5f, 0.75f));
    const uint32_t expected_specular = Moer::Pack_R8G8B8A8_Gamma_UFLOAT(float4(0.04f, 0.04f, 0.04f, 0.625f));
    const uint32_t expected_normal = static_cast<uint32_t>(
        Moer::Math::NdirToOctUnorm32(Normalizef(Moer::Unpack_Normal(packed_normal)))
    );
    std::array<uint32_t, pixel_count> expected_view_depth{};
    std::array<uint32_t, pixel_count> expected_clip_depth{};
    std::array<uint32_t, pixel_count> expected_diffuse_albedo{};
    std::array<uint32_t, pixel_count> expected_specular_roughness{};
    std::array<uint32_t, pixel_count> expected_normal_buffer{};
    expected_view_depth.fill(FloatBits(1.0f));
    expected_clip_depth.fill(FloatBits(0.0f));
    expected_diffuse_albedo.fill(expected_diffuse);
    expected_specular_roughness.fill(expected_specular);
    expected_normal_buffer.fill(expected_normal);

    std::array<uint32_t, pixel_count> view_depth_readback{};
    std::array<uint32_t, pixel_count> clip_depth_readback{};
    std::array<uint32_t, pixel_count> diffuse_readback{};
    std::array<uint32_t, pixel_count> specular_readback{};
    std::array<uint32_t, pixel_count> normal_readback{};
    std::array<uint8_t, pixel_count * 8> expected_zero_half{};
    std::array<uint8_t, pixel_count * 8> emission_readback{};
    std::array<uint8_t, pixel_count * 8> motion_readback{};
    std::array<std::array<uint32_t, pixel_count * 4u>, visualize_mode_count> visualize_readbacks{};

    auto build_visualize_expected = [&](uint32_t mode) {
        std::array<float4, pixel_count> expected{};
        const float3 expected_visual_normal = Moer::Math::OctToNdirUnorm32(expected_normal);
        const float expected_depth_visual = 1.0f - std::log2(2.0f) / 16.0f;
        const float expected_material = Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(expected_specular).w;

        for (uint32_t y = 0; y < viewport_height; ++y) {
            for (uint32_t x = 0; x < viewport_width; ++x) {
                const uint32_t pixel = y * viewport_width + x;
                switch (mode) {
                    case EFC_POSITION: {
                        const float clip_x = (static_cast<float>(x) + 0.5f) / viewport_width * 2.0f - 1.0f;
                        const float clip_y = 1.0f - (static_cast<float>(y) + 0.5f) / viewport_height * 2.0f;
                        expected[pixel] = float4(Frac(clip_x * 0.025f), Frac(clip_y * 0.025f), 0.0f, 1.0f);
                        break;
                    }
                    case EFC_NORMAL:
                        expected[pixel] = float4(expected_visual_normal * 0.5f + 0.5f, 1.0f);
                        break;
                    case EFC_VIEW_DEPTH:
                        expected[pixel] = float4(expected_depth_visual, expected_depth_visual, expected_depth_visual, 1.0f);
                        break;
                    case EFC_DEPTH:
                        expected[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
                        break;
                    case EFC_MATERIAL:
                        expected[pixel] = float4(expected_material, expected_material, expected_material, expected_material);
                        break;
                    case EFC_MOTION:
                        expected[pixel] = float4(0.5f, 0.5f, 0.0f, 1.0f);
                        break;
                    default:
                        expected[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
                        break;
                }
            }
        }
        return expected;
    };

    RaytracingGeometryInfo geometry_info{};
    geometry_info.build_flags = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
    geometry_info.vertex_format = PF_R32G32B32_SFLOAT;
    geometry_info.index_type = EIndexElementType::IET_UINT32;

    RaytracingSegment segment{};
    segment.vertex_offset = 0;
    segment.index_offset = 0;
    segment.first_vertex = 3;
    segment.vertex_count = 3;
    segment.vertex_stride = sizeof(float3);
    segment.first_primitive = 1;
    segment.primitive_count = 1;
    segment.vertex_buffer = position_buffer;
    segment.index_buffer = index_buffer;
    segment.type = RTGT_TRIANGLES;
    segment.flags = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    segment.b_force_opaque = false;
    segment.b_cull_back_face = false;
    segment.b_flip_face = false;
    geometry_info.segments.emplace_back(segment);

    RaytracingGeometryRef geometry = device.CreateRaytracingGeometry(geometry_info);
    RaytracingSceneRef scene = device.CreateRaytracingScene();
    scene->RegisterGeometry(geometry);
    RaytracingInstance& instance = scene->AddInstance();
    instance.geom = geometry;
    instance.transform = Matrix3x4f(
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f
    );
    instance.custom_index = expected_instance_index;
    instance.visible_mask = RTVM_ALL;
    scene->MarkModified(instance.instance_id);

    Raytracing::RTGBufferMacros gbuffer_macros{};
    gbuffer_macros.SetMutation<Raytracing::RaytracingGBufferPipeline::PRINT_TEST>(true);
    auto pipeline_object = ShaderManager::Get().Compute<Raytracing::RaytracingGBufferPipeline>(
        "pipelines/raytracing/passes/GBufferRT.hlsl",
        gbuffer_macros
    );
    auto pipeline = MakeShared<Raytracing::RaytracingGBufferPipeline>(std::move(pipeline_object));
    auto visualize_pipeline_object = ShaderManager::Get().Compute<Raytracing::VisualizePipeline>(
        "pipelines/raytracing/passes/VisualizePass.hlsl"
    );
    auto visualize_pipeline = MakeShared<Raytracing::VisualizePipeline>(std::move(visualize_pipeline_object));

    RenderGraph graph;
    RGBuffer* rg_constants = graph.ImportBuffer(MOER_TEXT("RGGBuffer.constants"), constants_buffer, EQueueType::Graphics);
    RGBuffer* rg_instances = graph.ImportBuffer(MOER_TEXT("RGGBuffer.instances"), instance_buffer, EQueueType::Graphics);
    RGBuffer* rg_primitives = graph.ImportBuffer(MOER_TEXT("RGGBuffer.primitives"), primitive_buffer, EQueueType::Graphics);
    RGBuffer* rg_materials = graph.ImportBuffer(MOER_TEXT("RGGBuffer.materials"), material_buffer, EQueueType::Graphics);
    RGBuffer* rg_positions = graph.ImportBuffer(MOER_TEXT("RGGBuffer.positions"), position_buffer, EQueueType::Graphics);
    RGBuffer* rg_normals = graph.ImportBuffer(MOER_TEXT("RGGBuffer.normals"), packed_normal_buffer, EQueueType::Graphics);
    RGBuffer* rg_texcoords = graph.ImportBuffer(MOER_TEXT("RGGBuffer.texcoords"), texcoord_buffer, EQueueType::Graphics);
    RGBuffer* rg_indices = graph.ImportBuffer(MOER_TEXT("RGGBuffer.indices"), index_buffer, EQueueType::Graphics);
    RGBuffer* rg_blas = ImportRaytracingGeometryBuffer(graph, MOER_TEXT("RGGBuffer.blas_buffer"), geometry);
    RGTexture* rg_view_depth = graph.ImportTexture(MOER_TEXT("RGGBuffer.view_depth"), view_depth, EQueueType::Graphics);
    RGTexture* rg_diffuse_albedo = graph.ImportTexture(MOER_TEXT("RGGBuffer.diffuse_albedo"), diffuse_albedo, EQueueType::Graphics);
    RGTexture* rg_specular_roughness = graph.ImportTexture(MOER_TEXT("RGGBuffer.specular_roughness"), specular_roughness, EQueueType::Graphics);
    RGTexture* rg_normal = graph.ImportTexture(MOER_TEXT("RGGBuffer.normal"), normal, EQueueType::Graphics);
    RGTexture* rg_emission = graph.ImportTexture(MOER_TEXT("RGGBuffer.emission"), emission, EQueueType::Graphics);
    RGTexture* rg_motion = graph.ImportTexture(MOER_TEXT("RGGBuffer.motion"), motion, EQueueType::Graphics);
    RGTexture* rg_clip_depth = graph.ImportTexture(MOER_TEXT("RGGBuffer.clip_depth"), clip_depth, EQueueType::Graphics);
    RGTexture* rg_direct_lighting = graph.ImportTexture(
        MOER_TEXT("RGGBufferVisualize.direct_lighting"), direct_lighting, EQueueType::Graphics
    );
    RGTexture* rg_diffuse_lighting = graph.ImportTexture(
        MOER_TEXT("RGGBufferVisualize.diffuse_lighting"), diffuse_lighting, EQueueType::Graphics
    );
    RGTexture* rg_specular_lighting = graph.ImportTexture(
        MOER_TEXT("RGGBufferVisualize.specular_lighting"), specular_lighting, EQueueType::Graphics
    );
    RGTexture* rg_normal_roughness = graph.ImportTexture(
        MOER_TEXT("RGGBufferVisualize.normal_roughness"), normal_roughness, EQueueType::Graphics
    );
    std::array<RGBuffer*, visualize_mode_count> rg_visualize_params_buffers{};
    std::array<RGTexture*, visualize_mode_count> rg_visualize_outputs{};
    for (uint32_t mode_index = 0; mode_index < visualize_mode_count; ++mode_index) {
        rg_visualize_params_buffers[mode_index] = graph.ImportBuffer(
            VisualizeParamsResourceName(visualize_modes[mode_index]),
            visualize_params_buffers[mode_index],
            EQueueType::Graphics
        );
        rg_visualize_outputs[mode_index] = graph.ImportTexture(
            VisualizeOutputResourceName(visualize_modes[mode_index]),
            visualize_outputs[mode_index],
            EQueueType::Graphics
        );
    }

    auto* upload_params = graph.Alloc<RGRaySceneUploadParams>();
    auto* upload_accesses = graph.Alloc<RGBufferAccessArray>(8);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_constants}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_instances}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_primitives}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_materials}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_positions}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_normals}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_texcoords}, EBufferState::TRANSFER_DST);
    upload_accesses->AddAccess(RGBufferView{.buffer = rg_indices}, EBufferState::TRANSFER_DST);
    upload_params->buffers = upload_accesses;
    graph.AddPass(
        MOER_TEXT("RGGBuffer.UploadSceneBuffers"),
        upload_params,
        ERGPassFlags::Graphics,
        [rg_constants, rg_instances, rg_primitives, rg_materials, rg_positions, rg_normals, rg_texcoords, rg_indices, constants, instances, primitives, materials, positions, packed_normals, texcoords, indices](RHICommandList& cmd_list, RGContext) {
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GBufferConstants>(std::span<const GBufferConstants>(&constants, 1)),
                rg_constants->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadConstants")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GInstance>(std::span<const GInstance>(instances.data(), instances.size())),
                rg_instances->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadInstances")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GPrimitive>(std::span<const GPrimitive>(primitives.data(), primitives.size())),
                rg_primitives->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadPrimitives")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<GMaterial>(std::span<const GMaterial>(materials.data(), materials.size())),
                rg_materials->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadMaterials")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<float3>(std::span<const float3>(positions.data(), positions.size())),
                rg_positions->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadPositions")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<uint32_t>(std::span<const uint32_t>(packed_normals.data(), packed_normals.size())),
                rg_normals->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadNormals")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<float2>(std::span<const float2>(texcoords.data(), texcoords.size())),
                rg_texcoords->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadTexcoords")
            );
            cmd_list.CopyFrom(
                MakeTypedUploadBytes<uint32_t>(std::span<const uint32_t>(indices.data(), indices.size())),
                rg_indices->RHI()->GetView(),
                MOER_TEXT("RGGBufferUploadIndices")
            );
        }
    );

    auto* build_blas_params = graph.Alloc<RGRayBuildBlasParams>();
    build_blas_params->vertices = RGBufferView{.buffer = rg_positions};
    build_blas_params->indices = RGBufferView{.buffer = rg_indices};
    build_blas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
    graph.AddPass(
        MOER_TEXT("RGGBuffer.BuildBLAS"),
        build_blas_params,
        ERGPassFlags::Graphics,
        [geometry](RHICommandList& cmd_list, RGContext) {
            cmd_list.BuildAccelerationStructures({AccelerationStructureBuildParam{
                .geometry = geometry,
                .mode = ERaytracingBuildMode::BUILD,
            }});
        }
    );

    auto* prepared_tlas_update = graph.Alloc<RGRayPreparedTlasUpdate>();
    prepared_tlas_update->command = scene->UpdateScene();
    if (!prepared_tlas_update->command) {
        LOG_ERROR(MOER_TEXT("RenderGraph GBuffer output test failed to prepare TLAS update command"));
        return 1;
    }

    RaytracingTlasRef tlas = scene->GetTlas();
    if (!tlas) {
        LOG_ERROR(MOER_TEXT("RenderGraph GBuffer output test prepared a null TLAS"));
        return 1;
    }
    RGBuffer* rg_tlas = ImportRaytracingTlasBuffer(graph, MOER_TEXT("RGGBuffer.tlas_buffer"), tlas);

    auto* tlas_params = graph.Alloc<RGRayTlasParams>();
    tlas_params->blas_buffer = RGBufferView{.buffer = rg_blas};
    tlas_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
    graph.AddPass(
        MOER_TEXT("RGGBuffer.BuildTLAS"),
        tlas_params,
        ERGPassFlags::Graphics,
        [prepared_tlas_update](RHICommandList& cmd_list, RGContext) {
            cmd_list.UpdateRaytracingScene(std::move(prepared_tlas_update->command));
        }
    );

    auto* update_params = graph.Alloc<RGBindlessUpdateParams>();
    graph.AddPass(
        MOER_TEXT("RGGBuffer.UpdateBindless"),
        update_params,
        ERGPassFlags::Graphics,
        [bindless_array](RHICommandList& cmd_list, RGContext) {
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                WriteBindlessArray{bindless_array, EBufferState::UNORDERED_ACCESS}
            );
            cmd_list.UpdateBindlessArray(bindless_array);
            cmd_list.Barriers(
                EQueueType::Graphics,
                EQueueType::Graphics,
                EPassType::Compute,
                ReadBindlessArray{bindless_array, EBufferState::SHADER_RESOURCE}
            );
        }
    );

    auto* trace_params = graph.Alloc<RGRayGBufferTraceParams>();
    auto* scene_read_accesses = graph.Alloc<RGBufferAccessArray>(7);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_instances}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_primitives}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_materials}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_positions}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_normals}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_texcoords}, EBufferState::SHADER_RESOURCE);
    scene_read_accesses->AddAccess(RGBufferView{.buffer = rg_indices}, EBufferState::SHADER_RESOURCE);
    trace_params->scene_buffers = scene_read_accesses;
    trace_params->constants = RGBufferView{.buffer = rg_constants};
    trace_params->view_depth = WholeTextureView(rg_view_depth);
    trace_params->diffuse_albedo = WholeTextureView(rg_diffuse_albedo);
    trace_params->specular_roughness = WholeTextureView(rg_specular_roughness);
    trace_params->normal = WholeTextureView(rg_normal);
    trace_params->emission = WholeTextureView(rg_emission);
    trace_params->motion = WholeTextureView(rg_motion);
    trace_params->clip_depth = WholeTextureView(rg_clip_depth);
    trace_params->tlas_buffer = RGBufferView{.buffer = rg_tlas};
    graph.AddPass(
        MOER_TEXT("RGGBuffer.Trace"),
        trace_params,
        ERGPassFlags::Graphics | ERGPassFlags::ComputeShader,
        [rg_constants, rg_view_depth, rg_diffuse_albedo, rg_specular_roughness, rg_normal, rg_emission, rg_motion, rg_clip_depth, tlas, bindless_array, pipeline, bindless_params](RHICommandList& cmd_list, RGContext) mutable {
            cmd_list
                .Compute(
                    *pipeline,
                    bindless_params,
                    rg_constants->RHI()->GetView(),
                    rg_view_depth->RHI()->GetView(),
                    rg_diffuse_albedo->RHI()->GetView(),
                    rg_specular_roughness->RHI()->GetView(),
                    rg_normal->RHI()->GetView(),
                    rg_emission->RHI()->GetView(),
                    rg_motion->RHI()->GetView(),
                    rg_clip_depth->RHI()->GetView(),
                    tlas,
                    bindless_array
                )
                .Dispatch(uint3(1, 1, 1), MOER_TEXT("RGGBufferTrace"));
        }
    );

    for (uint32_t mode_index = 0; mode_index < visualize_mode_count; ++mode_index) {
        VisualizeParams visualize_params{};
        visualize_params.main_view = constants.main_view;
        visualize_params.output_size = uint2(viewport_width, viewport_height);
        visualize_params.resolution_scale = float2(1.0f, 1.0f);
        visualize_params.visualize_mode = visualize_modes[mode_index];
        visualize_params.b_split = 0u;
        visualize_params.split_ratio = 0.5f;

        auto* visualize_upload_params = graph.Alloc<RGBufferUploadParams>();
        visualize_upload_params->target = RGBufferView{.buffer = rg_visualize_params_buffers[mode_index]};
        graph.AddPass(
            MOER_TEXT("RGGBufferVisualize.UploadConstants"),
            visualize_upload_params,
            ERGPassFlags::Graphics,
            [rg_params = rg_visualize_params_buffers[mode_index], visualize_params](RHICommandList& cmd_list, RGContext) {
                cmd_list.CopyFrom(
                    MakeTypedUploadBytes<VisualizeParams>(std::span<const VisualizeParams>(&visualize_params, 1)),
                    rg_params->RHI()->GetView(),
                    MOER_TEXT("RGGBufferVisualizeUploadConstants")
                );
            }
        );

        auto* visualize_params_access = graph.Alloc<RGGBufferVisualizeParams>();
        visualize_params_access->constants = RGBufferView{.buffer = rg_visualize_params_buffers[mode_index]};
        visualize_params_access->ldr_color = WholeTextureView(rg_direct_lighting);
        visualize_params_access->diffuse_lighting = WholeTextureView(rg_diffuse_lighting);
        visualize_params_access->specular_lighting = WholeTextureView(rg_specular_lighting);
        visualize_params_access->view_depth = WholeTextureView(rg_view_depth);
        visualize_params_access->clip_depth = WholeTextureView(rg_clip_depth);
        visualize_params_access->emission = WholeTextureView(rg_emission);
        visualize_params_access->normal = WholeTextureView(rg_normal);
        visualize_params_access->specular_roughness = WholeTextureView(rg_specular_roughness);
        visualize_params_access->motion = WholeTextureView(rg_motion);
        visualize_params_access->normal_roughness = WholeTextureView(rg_normal_roughness);
        visualize_params_access->prev_view_depth = WholeTextureView(rg_view_depth);
        visualize_params_access->output = WholeTextureView(rg_visualize_outputs[mode_index]);
        graph.AddPass(
            MOER_TEXT("RGGBufferVisualize.Dispatch"),
            visualize_params_access,
            ERGPassFlags::Graphics | ERGPassFlags::ComputeShader,
            [rg_params = rg_visualize_params_buffers[mode_index],
             rg_output = rg_visualize_outputs[mode_index],
             rg_direct_lighting,
             rg_diffuse_lighting,
             rg_specular_lighting,
             rg_view_depth,
             rg_clip_depth,
             rg_emission,
             rg_normal,
             rg_specular_roughness,
             rg_motion,
             rg_normal_roughness,
             visualize_pipeline](RHICommandList& cmd_list, RGContext) mutable {
                cmd_list
                    .Compute(
                        *visualize_pipeline,
                        rg_params->RHI()->GetView(),
                        rg_direct_lighting->RHI()->GetView(),
                        rg_diffuse_lighting->RHI()->GetView(),
                        rg_specular_lighting->RHI()->GetView(),
                        rg_view_depth->RHI()->GetView(),
                        rg_clip_depth->RHI()->GetView(),
                        rg_emission->RHI()->GetView(),
                        rg_normal->RHI()->GetView(),
                        rg_specular_roughness->RHI()->GetView(),
                        rg_motion->RHI()->GetView(),
                        rg_normal_roughness->RHI()->GetView(),
                        rg_view_depth->RHI()->GetView(),
                        rg_output->RHI()->GetView()
                    )
                    .Dispatch(uint3(1, 1, 1), MOER_TEXT("RGGBufferVisualizeDispatch"));
            }
        );
    }

    auto* readback_state_params = graph.Alloc<RGGBufferTextureReadbackStateParams>();
    auto* readback_accesses = graph.Alloc<RGTextureAccessArray>(7 + visualize_mode_count);
    readback_accesses->AddAccess(WholeTextureView(rg_view_depth), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_diffuse_albedo), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_specular_roughness), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_normal), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_emission), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_motion), ETextureState::TRANSFER_SRC);
    readback_accesses->AddAccess(WholeTextureView(rg_clip_depth), ETextureState::TRANSFER_SRC);
    for (RGTexture* rg_output : rg_visualize_outputs) {
        readback_accesses->AddAccess(WholeTextureView(rg_output), ETextureState::TRANSFER_SRC);
    }
    readback_state_params->textures = readback_accesses;
    graph.AddPass(
        MOER_TEXT("RGGBuffer.PrepareReadback"),
        readback_state_params,
        ERGPassFlags::Graphics,
        [](RHICommandList&, RGContext) {}
    );

    graph.ExportBuffer(rg_constants, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_instances, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_primitives, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_materials, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_positions, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
    graph.ExportBuffer(rg_normals, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_texcoords, EBufferState::SHADER_RESOURCE, EQueueType::Graphics);
    graph.ExportBuffer(rg_indices, EBufferState::ACCELERATION_STRUCTURE_BUILD_INPUT, EQueueType::Graphics);
    graph.ExportTexture(rg_view_depth, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_diffuse_albedo, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_specular_roughness, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_normal, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_emission, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_motion, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    graph.ExportTexture(rg_clip_depth, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    for (RGTexture* rg_output : rg_visualize_outputs) {
        graph.ExportTexture(rg_output, ETextureState::TRANSFER_SRC, EQueueType::Graphics);
    }
    graph.Dispatch();

    CommandList readback_cmd(EQueueType::Graphics);
    readback_cmd.Barriers(
        {
            BarrierCreateInfo::Transition(view_depth->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(diffuse_albedo->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(specular_roughness->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(normal->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(emission->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(motion->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy),
            BarrierCreateInfo::Transition(clip_depth->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy)
        },
        EQueueType::Graphics,
        EQueueType::Graphics
    );
    for (TextureRef& visualize_output : visualize_outputs) {
        readback_cmd.Barriers(
            {BarrierCreateInfo::Transition(
                visualize_output->GetView(), ETextureState::TRANSFER_SRC, ETextureState::TRANSFER_SRC, EPassType::Copy
            )},
            EQueueType::Graphics,
            EQueueType::Graphics
        );
    }
    SyncPointRef view_depth_event = readback_cmd.ReadbackCopy(
        view_depth->GetView(),
        ToByteSpan(view_depth_readback),
        MOER_TEXT("RGGBufferReadbackViewDepth")
    );
    SyncPointRef clip_depth_event = readback_cmd.ReadbackCopy(
        clip_depth->GetView(),
        ToByteSpan(clip_depth_readback),
        MOER_TEXT("RGGBufferReadbackClipDepth")
    );
    SyncPointRef diffuse_event = readback_cmd.ReadbackCopy(
        diffuse_albedo->GetView(),
        ToByteSpan(diffuse_readback),
        MOER_TEXT("RGGBufferReadbackDiffuseAlbedo")
    );
    SyncPointRef specular_event = readback_cmd.ReadbackCopy(
        specular_roughness->GetView(),
        ToByteSpan(specular_readback),
        MOER_TEXT("RGGBufferReadbackSpecularRoughness")
    );
    SyncPointRef normal_event = readback_cmd.ReadbackCopy(
        normal->GetView(),
        ToByteSpan(normal_readback),
        MOER_TEXT("RGGBufferReadbackNormal")
    );
    SyncPointRef emission_event = readback_cmd.ReadbackCopy(
        emission->GetView(),
        ToByteSpan(emission_readback),
        MOER_TEXT("RGGBufferReadbackEmission")
    );
    SyncPointRef motion_event = readback_cmd.ReadbackCopy(
        motion->GetView(),
        ToByteSpan(motion_readback),
        MOER_TEXT("RGGBufferReadbackMotion")
    );
    std::array<SyncPointRef, visualize_mode_count> visualize_events{};
    for (uint32_t mode_index = 0; mode_index < visualize_mode_count; ++mode_index) {
        visualize_events[mode_index] = readback_cmd.ReadbackCopy(
            visualize_outputs[mode_index]->GetView(),
            ToByteSpan(visualize_readbacks[mode_index]),
            VisualizeModeLabel(visualize_modes[mode_index])
        );
    }

    Array<CommandList> command_lists{};
    command_lists.emplace_back(std::move(readback_cmd));
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    if (view_depth_event) {
        view_depth_event->WaitHost();
    }
    if (clip_depth_event) {
        clip_depth_event->WaitHost();
    }
    if (diffuse_event) {
        diffuse_event->WaitHost();
    }
    if (specular_event) {
        specular_event->WaitHost();
    }
    if (normal_event) {
        normal_event->WaitHost();
    }
    if (emission_event) {
        emission_event->WaitHost();
    }
    if (motion_event) {
        motion_event->WaitHost();
    }
    for (SyncPointRef& visualize_event : visualize_events) {
        if (visualize_event) {
            visualize_event->WaitHost();
        }
    }

    if (!ValidateWords(expected_view_depth, view_depth_readback, MOER_TEXT("RenderGraphGBuffer.ViewDepth"))) {
        return 1;
    }
    if (!ValidateWords(expected_clip_depth, clip_depth_readback, MOER_TEXT("RenderGraphGBuffer.ClipDepth"))) {
        return 1;
    }
    if (!ValidateWords(expected_diffuse_albedo, diffuse_readback, MOER_TEXT("RenderGraphGBuffer.DiffuseAlbedo"))) {
        return 1;
    }
    if (!ValidateWords(expected_specular_roughness, specular_readback, MOER_TEXT("RenderGraphGBuffer.SpecularRoughness"))) {
        return 1;
    }
    if (!ValidateWords(expected_normal_buffer, normal_readback, MOER_TEXT("RenderGraphGBuffer.Normal"))) {
        return 1;
    }
    if (!ValidateBytes(expected_zero_half, emission_readback, MOER_TEXT("RenderGraphGBuffer.Emission"))) {
        return 1;
    }
    if (!ValidateBytes(expected_zero_half, motion_readback, MOER_TEXT("RenderGraphGBuffer.Motion"))) {
        return 1;
    }
    for (uint32_t mode_index = 0; mode_index < visualize_mode_count; ++mode_index) {
        const auto expected_visualize = build_visualize_expected(visualize_modes[mode_index]);
        if (!ValidateFloat4Words(expected_visualize, visualize_readbacks[mode_index], VisualizeModeLabel(visualize_modes[mode_index]))) {
            return 1;
        }
    }

    scene->AdvanceFrame();

    LOG_INFO(
        MOER_TEXT("RenderGraph ray query GBuffer output and visualize readback test passed, viewport={}x{}"),
        viewport_width,
        viewport_height
    );
    return 0;
}

} // namespace Moer::Render::Tests