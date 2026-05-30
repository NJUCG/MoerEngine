#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "math/Matrix.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render::Tests {
namespace {

constexpr uint32_t baseline_element_count = 128;
constexpr uint32_t baseline_texture_size  = 16;
constexpr uint32_t baseline_pixel_count   = baseline_texture_size * baseline_texture_size;

struct RGBaselineComputeArgs {
    uint32_t element_count;
    uint32_t addend;
    uint32_t xor_mask;
    uint32_t pad;
};

struct RGBaselineRasterArgs {
    float4 color;
};

class RGBaselineComputePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RGBaselineComputePipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(RGBaselineComputeArgs, args);
    DEFINE_SHADER_BUFFER(src);
    DEFINE_SHADER_BUFFER(dst);

    DEFINE_SHADER_ARGS(args, src, dst);
};

class RGBaselineRayQueryPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RGBaselineRayQueryPipeline);

    DEFINE_SHADER_TLAS(tlas);

    DEFINE_SHADER_ARGS(tlas);
};

class RGBaselineRasterPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(RGBaselineRasterPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(RGBaselineRasterArgs, args);

    DEFINE_SHADER_ARGS(args);
};

template<typename T>
std::span<byte> ToByteSpan(std::vector<T>& values) {
    return std::span<byte>(reinterpret_cast<byte*>(values.data()), values.size() * sizeof(T));
}

template<typename T, size_t Size>
std::span<byte> ToByteSpan(std::array<T, Size>& values) {
    return std::span<byte>(reinterpret_cast<byte*>(values.data()), values.size() * sizeof(T));
}

void SubmitAndWait(Array<CommandList>&& command_lists) {
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
}

std::vector<uint8_t> MakeRgba8Pattern(uint32_t width, uint32_t height, uint32_t seed) {
    std::vector<uint8_t> bytes(size_t(width) * size_t(height) * 4u, 0u);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t pixel = size_t(y) * size_t(width) + x;
            bytes[pixel * 4u + 0u] = static_cast<uint8_t>((x * 17u + seed) & 0xffu);
            bytes[pixel * 4u + 1u] = static_cast<uint8_t>((y * 23u + seed * 3u) & 0xffu);
            bytes[pixel * 4u + 2u] = static_cast<uint8_t>(((x ^ y) * 11u + seed * 5u) & 0xffu);
            bytes[pixel * 4u + 3u] = 255u;
        }
    }
    return bytes;
}

bool ValidateBuffer(
    std::span<const uint32_t> expected,
    std::span<const uint32_t> actual,
    std::string_view          label
) {
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

bool ValidateBytes(
    std::span<const uint8_t> expected,
    std::span<const uint8_t> actual,
    std::string_view         label
) {
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            LOG_ERROR(
                MOER_TEXT("{} byte mismatch at index={}, expected={}, got={}"),
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

bool ValidateSolidRedTexture(std::span<const uint8_t> actual, std::string_view label) {
    for (uint32_t pixel = 0; pixel < baseline_pixel_count; ++pixel) {
        const size_t offset = size_t(pixel) * 4u;
        const bool valid = actual[offset + 0u] == 255u && actual[offset + 1u] == 0u &&
                           actual[offset + 2u] == 0u && actual[offset + 3u] == 255u;
        if (!valid) {
            LOG_ERROR(
                MOER_TEXT("{} pixel mismatch at pixel={}, rgba=({}, {}, {}, {})"),
                label,
                pixel,
                actual[offset + 0u],
                actual[offset + 1u],
                actual[offset + 2u],
                actual[offset + 3u]
            );
            return false;
        }
    }
    return true;
}

bool IsRaytracingCoverageSupported() {
    auto& vk_device = static_cast<VulkanDevice&>(*RenderDevice::Get().GetImpl());
    const auto& optional_extensions = vk_device.GetOptionalExtensions();
    return optional_extensions.m_has_khr_acceleration_structure && optional_extensions.m_has_khr_ray_query;
}

RGBaselineRasterPipeline CreateBaselineRasterPipeline() {
    VertexStream stream{};
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        stream,
        {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
    );

    return ShaderManager::Get()
        .Raster()
        .Vertex("tests/RGBaselineFullscreen.vert.hlsl")
        .Pixel("tests/RGBaselineColor.frag.hlsl")
        .Build<RGBaselineRasterPipeline>(std::move(pso_info));
}

bool RecordRaytracingCoverageIfSupported(
    CommandList&                 command_list,
    RGBaselineRayQueryPipeline*   ray_query_pipeline,
    BufferRef                     vertex_buffer,
    BufferRef                     index_buffer,
    RaytracingGeometryRef&        retained_geometry,
    RaytracingSceneRef&           retained_scene
) {
    if (ray_query_pipeline == nullptr) {
        LOG_INFO(
            MOER_TEXT("[TESTCASE][SKIP] RHICommandListRGBaselineRaytracing :: ray query is unavailable")
        );
        return false;
    }

    RaytracingGeometryInfo geometry_info{};
    geometry_info.build_flags   = ERayTracingAccelerationStructureBuildFlags::PREFER_FAST_TRACE;
    geometry_info.vertex_format = PF_R32G32B32_SFLOAT;
    geometry_info.index_type    = EIndexElementType::IET_UINT32;

    RaytracingSegment segment{};
    segment.vertex_offset    = 0;
    segment.index_offset     = 0;
    segment.first_vertex     = 0;
    segment.vertex_count     = 3;
    segment.vertex_stride    = sizeof(float3);
    segment.first_primitive  = 0;
    segment.primitive_count  = 1;
    segment.vertex_buffer    = vertex_buffer;
    segment.index_buffer     = index_buffer;
    segment.type             = RTGT_TRIANGLES;
    segment.flags            = ERayTracingGeometryFlags::GEOMETRY_OPAQUE;
    segment.b_force_opaque   = false;
    segment.b_cull_back_face = false;
    segment.b_flip_face      = false;
    geometry_info.segments.emplace_back(segment);

    auto geometry = RenderDevice::Get().CreateRaytracingGeometry(geometry_info);
    auto scene    = RenderDevice::Get().CreateRaytracingScene();
    scene->RegisterGeometry(geometry);
    retained_geometry = geometry;
    retained_scene    = scene;

    RaytracingInstance& instance = scene->AddInstance();
    instance.geom                = geometry;
    instance.transform           = Matrix3x4f(
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

    command_list.BuildAccelerationStructures({AccelerationStructureBuildParam{
        .geometry = geometry,
        .mode     = ERaytracingBuildMode::BUILD,
    }});
    command_list.UpdateRaytracingScene(scene);
    command_list.Compute(*ray_query_pipeline, scene->GetTlas()).Dispatch(1u, MOER_TEXT("RGBaselineRayQuery"));
    return true;
}

} // namespace

int RunRHICommandListRGBaselineTest() {
    auto& device = RenderDevice::Get();

    auto src_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_src"),
        baseline_element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto compute_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_compute"),
        baseline_element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto copied_compute_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_copied_compute"),
        baseline_element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto pixel_upload_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_pixel_upload_buffer"),
        baseline_pixel_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );
    auto scratch_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_scratch"),
        baseline_element_count,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto vertex_buffer = device.CreateBuffer<float3>(
        MOER_TEXT("rg_baseline_rt_vertices"),
        3,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::VERTEX_BUFFER
    );
    auto index_buffer = device.CreateBuffer<uint32_t>(
        MOER_TEXT("rg_baseline_rt_indices"),
        3,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT | EBufferUsageFlags::INDEX_BUFFER
    );

    auto upload_texture = device.CreateTexture(
        MOER_TEXT("rg_baseline_upload_texture"),
        Extent2D(baseline_texture_size, baseline_texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::SAMPLED
    );
    auto copied_texture = device.CreateTexture(
        MOER_TEXT("rg_baseline_copied_texture"),
        Extent2D(baseline_texture_size, baseline_texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::SAMPLED
    );
    auto buffer_texture = device.CreateTexture(
        MOER_TEXT("rg_baseline_buffer_texture"),
        Extent2D(baseline_texture_size, baseline_texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::SAMPLED
    );
    auto raster_target = device.CreateTexture(
        MOER_TEXT("rg_baseline_raster_target"),
        Extent2D(baseline_texture_size, baseline_texture_size),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC
    );

    auto compute_pipeline = ShaderManager::Get().Compute<RGBaselineComputePipeline>(
        "tests/RGBaselineCompute.comp.hlsl"
    );
    RGBaselineRayQueryPipeline ray_query_pipeline{};
    RGBaselineRayQueryPipeline* ray_query_pipeline_ptr = nullptr;
    if (IsRaytracingCoverageSupported()) {
        ray_query_pipeline = ShaderManager::Get().Compute<RGBaselineRayQueryPipeline>(
            "tests/RayQueryMinimal.comp.hlsl"
        );
        ray_query_pipeline_ptr = &ray_query_pipeline;
    }
    auto raster_pipeline = CreateBaselineRasterPipeline();

    std::vector<uint32_t> src_values(baseline_element_count);
    std::vector<uint32_t> expected_compute(baseline_element_count);
    std::vector<uint32_t> readback_compute(baseline_element_count, 0u);
    for (uint32_t i = 0; i < baseline_element_count; ++i) {
        src_values[i]       = 0x10000u + i * 13u + 5u;
        expected_compute[i] = (src_values[i] + 17u) ^ 0x5a5a00ffu;
    }

    std::vector<uint8_t> upload_texture_bytes = MakeRgba8Pattern(baseline_texture_size, baseline_texture_size, 9u);
    std::vector<uint8_t> copied_texture_readback(upload_texture_bytes.size(), 0u);
    std::vector<uint8_t> buffer_texture_readback(upload_texture_bytes.size(), 0u);
    std::vector<uint8_t> raster_readback(upload_texture_bytes.size(), 0u);
    std::array<float3, 3> rt_vertices{
        float3(-0.5f, -0.5f, 0.0f),
        float3(0.5f, -0.5f, 0.0f),
        float3(0.0f, 0.5f, 0.0f),
    };
    std::array<uint32_t, 3> rt_indices{0u, 1u, 2u};

    CommandList setup_cmd(EQueueType::Graphics);
    {
        auto copy_scope = setup_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(src_values), src_buffer->GetView(), MOER_TEXT("RGBaselineUploadSourceBuffer"));
        copy_scope.CopyFrom(ToByteSpan(upload_texture_bytes), upload_texture->GetView(), MOER_TEXT("RGBaselineUploadTexture"));
        copy_scope.CopyFrom(ToByteSpan(upload_texture_bytes), pixel_upload_buffer->GetView(), MOER_TEXT("RGBaselineUploadPixelBuffer"));
        copy_scope.CopyFrom(ToByteSpan(rt_vertices), vertex_buffer->GetView(), MOER_TEXT("RGBaselineUploadRTVertices"));
        copy_scope.CopyFrom(ToByteSpan(rt_indices), index_buffer->GetView(), MOER_TEXT("RGBaselineUploadRTIndices"));
    }
    setup_cmd.PushScopeWithTimeScope(MOER_TEXT("RGBaseline.SetupCopy"));
    setup_cmd.ClearResource(scratch_buffer->GetView(), 0u);
    setup_cmd.ClearResource(compute_buffer->GetView(), 0u);
    setup_cmd.ClearResource(copied_compute_buffer->GetView(), 0u);
    setup_cmd.ClearResource(raster_target->GetView(), float4(0.0f, 0.0f, 0.0f, 1.0f));
    setup_cmd.CopyFrom(upload_texture->GetView(), copied_texture->GetView(), MOER_TEXT("RGBaselineTextureToTexture"));
    setup_cmd.CopyFrom(pixel_upload_buffer->GetView(), buffer_texture->GetView(), MOER_TEXT("RGBaselineBufferToTexture"));
    setup_cmd.PopScopeWithTimeScope();

    CommandList compute_cmd(EQueueType::Graphics);
    compute_cmd.PushScopeWithTimeScope(MOER_TEXT("RGBaseline.ComputePass"));
    auto compute_query = compute_cmd.BeginTimestampQuery(MOER_TEXT("RGBaselineComputeTimestamp"));
    compute_cmd
        .Compute(
            compute_pipeline,
            RGBaselineComputeArgs{
                .element_count = baseline_element_count,
                .addend        = 17u,
                .xor_mask      = 0x5a5a00ffu,
                .pad           = 0u,
            },
            src_buffer->GetView(),
            compute_buffer->GetView()
        )
        .Dispatch((baseline_element_count + 63u) / 64u, MOER_TEXT("RGBaselineComputeDispatch"));
    compute_cmd.EndTimestampQuery(compute_query);
    compute_cmd.CopyFrom(compute_buffer->GetView(), copied_compute_buffer->GetView(), MOER_TEXT("RGBaselineBufferToBuffer"));
    compute_cmd.PopScopeWithTimeScope();

    CommandList graphics_cmd(EQueueType::Graphics);
    graphics_cmd.PushScopeWithTimeScope(MOER_TEXT("RGBaseline.GraphicsPass"));
    Array<SingleDrawParam> draw_params;
    draw_params.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
    graphics_cmd
        .Gfx(
            raster_pipeline,
            RGBaselineRasterArgs{
                .color = float4(1.0f, 0.0f, 0.0f, 1.0f),
            }
        )
        .Draw(
            MOER_TEXT("RGBaselineFullscreenDraw"),
            Rect2D(0, 0, baseline_texture_size, baseline_texture_size),
            std::move(draw_params),
            ColorAttachment(raster_target)
        );
    graphics_cmd.PopScopeWithTimeScope();

    CommandList raytracing_cmd(EQueueType::Graphics);
    raytracing_cmd.PushScopeWithTimeScope(MOER_TEXT("RGBaseline.RaytracingPass"));
    RaytracingGeometryRef retained_raytracing_geometry;
    RaytracingSceneRef retained_raytracing_scene;
    const bool raytracing_recorded = RecordRaytracingCoverageIfSupported(
        raytracing_cmd,
        ray_query_pipeline_ptr,
        vertex_buffer,
        index_buffer,
        retained_raytracing_geometry,
        retained_raytracing_scene
    );
    raytracing_cmd.PopScopeWithTimeScope();

    CommandList readback_cmd(EQueueType::Graphics);
    readback_cmd.PushScopeWithTimeScope(MOER_TEXT("RGBaseline.ReadbackPass"));
    readback_cmd.Barriers(
        {
            BarrierCreateInfo::Transition(
                copied_compute_buffer->GetView(),
                EBufferState::TRANSFER_DST,
                EBufferState::TRANSFER_SRC,
                EPassType::Copy
            ),
            BarrierCreateInfo::Transition(
                copied_texture->GetView(),
                ETextureState::TRANSFER_DST,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            ),
            BarrierCreateInfo::Transition(
                buffer_texture->GetView(),
                ETextureState::TRANSFER_DST,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            ),
            BarrierCreateInfo::Transition(
                raster_target->GetView(),
                ETextureState::RENDER_TARGET,
                ETextureState::TRANSFER_SRC,
                EPassType::Copy
            )
        },
        EQueueType::Graphics,
        EQueueType::Graphics
    );
    SyncPointRef compute_readback_event = readback_cmd.ReadbackCopy(
        copied_compute_buffer->GetView(),
        ToByteSpan(readback_compute),
        MOER_TEXT("RGBaselineReadbackComputeBuffer")
    );
    SyncPointRef copied_texture_event = readback_cmd.ReadbackCopy(
        copied_texture->GetView(),
        ToByteSpan(copied_texture_readback),
        MOER_TEXT("RGBaselineReadbackCopiedTexture")
    );
    SyncPointRef buffer_texture_event = readback_cmd.ReadbackCopy(
        buffer_texture->GetView(),
        ToByteSpan(buffer_texture_readback),
        MOER_TEXT("RGBaselineReadbackBufferTexture")
    );
    SyncPointRef raster_readback_event = readback_cmd.ReadbackCopy(
        raster_target->GetView(),
        ToByteSpan(raster_readback),
        MOER_TEXT("RGBaselineReadbackRasterTarget")
    );
    readback_cmd.PopScopeWithTimeScope();

    Array<CommandList> command_lists;
    command_lists.emplace_back(std::move(setup_cmd));
    command_lists.emplace_back(std::move(compute_cmd));
    command_lists.emplace_back(std::move(graphics_cmd));
    if (raytracing_recorded) {
        command_lists.emplace_back(std::move(raytracing_cmd));
    }
    command_lists.emplace_back(std::move(readback_cmd));

    const auto begin_time = std::chrono::steady_clock::now();
    SubmitAndWait(std::move(command_lists));
    if (compute_readback_event) {
        compute_readback_event->WaitHost();
    }
    if (copied_texture_event) {
        copied_texture_event->WaitHost();
    }
    if (buffer_texture_event) {
        buffer_texture_event->WaitHost();
    }
    if (raster_readback_event) {
        raster_readback_event->WaitHost();
    }
    const auto end_time = std::chrono::steady_clock::now();

    if (!ValidateBuffer(expected_compute, readback_compute, "RGBaselineCompute")) {
        return 1;
    }
    if (!ValidateBytes(upload_texture_bytes, copied_texture_readback, "RGBaselineTextureToTexture")) {
        return 1;
    }
    if (!ValidateBytes(upload_texture_bytes, buffer_texture_readback, "RGBaselineBufferToTexture")) {
        return 1;
    }
    if (!ValidateSolidRedTexture(raster_readback, "RGBaselineGraphics")) {
        return 1;
    }

    const QueryResult compute_query_result = compute_query.GetFuture().Get();
    if (compute_query_result.status != QueryStatus::Ready) {
        LOG_ERROR(MOER_TEXT("RGBaseline compute timestamp query did not resolve"));
        return 1;
    }

    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - begin_time).count();
    LOG_INFO(
        MOER_TEXT("RHICommandList RG baseline passed, elapsed_us={}, raytracing_recorded={}"),
        elapsed_us,
        raytracing_recorded
    );
    return 0;
}

} // namespace Moer::Render::Tests
