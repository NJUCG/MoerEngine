#include "TessellatedSurfacePass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Moer::Render::Raster {
namespace {

constexpr std::string_view kSurfaceShaderPath =
    "pipelines/raster/deferred/geometry/TessellatedSurface.hlsl";
// Matches the tested 16x16 grid at factor 64. Higher grid resolutions automatically receive a
// lower factor cap so UI combinations cannot grow tessellation work quadratically into a TDR.
constexpr uint64_t kSurfaceTriangleBudget = 2ull * 16ull * 16ull * 64ull * 64ull;

struct SurfacePreset {
    float  macro_amplitude;
    float  detail_amplitude;
    float  macro_frequency;
    float  detail_frequency;
    float  warp_strength;
    float  normal_epsilon;
    float4 low_color_roughness;
    float4 high_color;
};

SurfacePreset GetPreset(int preset) {
    if (preset == 1) {
        return SurfacePreset{
            .macro_amplitude    = 0.38f,
            .detail_amplitude   = 0.025f,
            .macro_frequency    = 0.12f,
            .detail_frequency   = 0.85f,
            .warp_strength      = 0.35f,
            .normal_epsilon     = 0.06f,
            .low_color_roughness = float4(0.58f, 0.70f, 0.84f, 0.68f),
            .high_color         = float4(0.98f, 0.99f, 1.0f, 0.0f)
        };
    }

    return SurfacePreset{
        .macro_amplitude    = 0.85f,
        .detail_amplitude   = 0.06f,
        .macro_frequency    = 0.09f,
        .detail_frequency   = 1.20f,
        .warp_strength      = 1.25f,
        .normal_epsilon     = 0.08f,
        .low_color_roughness = float4(0.43f, 0.18f, 0.045f, 0.82f),
        .high_color         = float4(0.95f, 0.67f, 0.27f, 0.0f)
    };
}

} // namespace

TessellatedSurfacePass::TessellatedSurfacePass(RasterContext& context) {
    if (!context.device.SupportsTessellation()) {
        LOG_WARNING(
            "[TessellatedSurface] Device does not support hardware tessellation; the showcase pass is disabled."
        );
        return;
    }

    device_max_tessellation_factor = context.device.GetMaxTessellationFactor();
    if (device_max_tessellation_factor < 2u) {
        LOG_WARNING(
            "[TessellatedSurface] Invalid device tessellation limit {}; the showcase pass is disabled.",
            device_max_tessellation_factor
        );
        return;
    }

    surface_data_buffer = context.device.CreateBuffer<byte>(
        "Raster::TessellatedSurfaceData",
        sizeof(TessellatedSurfaceData),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::CONSTANT_BUFFER
    );

    GfxPsoCreateInfo pipeline_info(
        RHIRasterizeInfo::Preset(),
        {},
        {
            RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM),
            RHIColorAttachmentInfo::Preset(PF_A2R10G10B10_UNORM_PACK32),
            RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)
        },
        RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
        context.textures.depth_linear_sampler.tex->GetFormat(),
        EPrimitiveTopology::PATCH_LIST
    );
    pipeline_info.patch_control_points = 3;

    pipeline = context.manager.Raster()
                   .Vertex(kSurfaceShaderPath, "SurfaceVS")
                   .Hull(kSurfaceShaderPath, "SurfaceHS")
                   .Domain(kSurfaceShaderPath, "SurfaceDS")
                   .Pixel(kSurfaceShaderPath, "SurfacePS")
                   .Build<TessellatedSurfacePipeline>(std::move(pipeline_info));

    supported = pipeline.handle.IsValid();
    LOG_INFO(
        "[TessellatedSurface] Hardware tessellation showcase {} (device max factor {}).",
        supported ? "ready" : "failed to create a pipeline",
        device_max_tessellation_factor
    );
}

void TessellatedSurfacePass::Process(
    RasterContext&     context,
    const RasterConfig& config,
    const Camera&       camera
) {
    if (!supported || !config.tessellated_surface_enabled) {
        return;
    }

    const uint32_t grid_resolution = static_cast<uint32_t>(
        std::clamp(config.tessellated_surface_grid_resolution, 1, 64)
    );
    const float half_extent = std::max(config.tessellated_surface_half_extent, 0.25f);
    const uint64_t patch_count = 2ull * grid_resolution * grid_resolution;
    const float budget_factor_limit = std::max(
        2.0f,
        std::floor(std::sqrt(static_cast<float>(kSurfaceTriangleBudget) / patch_count))
    );
    const float maximum_tessellation = std::clamp(
        config.tessellated_surface_max_tess_factor,
        2.0f,
        std::min(static_cast<float>(device_max_tessellation_factor), budget_factor_limit)
    );
    const float minimum_tessellation = std::clamp(
        config.tessellated_surface_min_tess_factor,
        2.0f,
        maximum_tessellation
    );

    const int preset_index = std::clamp(config.tessellated_surface_preset, 0, 1);
    const SurfacePreset preset = GetPreset(preset_index);
    const float height_scale = std::max(config.tessellated_surface_height_scale, 0.0f);
    const float detail_scale = std::max(config.tessellated_surface_detail_scale, 0.0f);
    const float wind_radians =
        config.tessellated_surface_wind_angle_degrees * 3.14159265358979323846f / 180.0f;
    const Vector3f camera_position = camera.GetPosition();
    const uint2 resolution = context.GetResolution();

    TessellatedSurfaceData data{};
    data.world2clip = Transpose(camera.GetViewProjectionMatrix());
    data.camera_position_tan_half_fov = float4(
        camera_position.x,
        camera_position.y,
        camera_position.z,
        camera.GetTanHalfFov()
    );
    data.viewport_tessellation = float4(
        static_cast<float>(resolution.x),
        static_cast<float>(resolution.y),
        std::max(config.tessellated_surface_target_edge_pixels, 1.0f),
        maximum_tessellation
    );
    data.surface_origin_extent = float4(
        config.tessellated_surface_center.x,
        config.tessellated_surface_center.y,
        config.tessellated_surface_center.z,
        half_extent
    );
    data.displacement = float4(
        preset.macro_amplitude * height_scale,
        preset.detail_amplitude * detail_scale,
        preset.macro_frequency,
        preset.detail_frequency
    );
    data.wind_and_normal = float4(
        std::cos(wind_radians),
        std::sin(wind_radians),
        preset.warp_strength,
        preset.normal_epsilon
    );
    data.color_low_roughness = preset.low_color_roughness;
    data.color_high_min_tessellation = float4(
        preset.high_color.x,
        preset.high_color.y,
        preset.high_color.z,
        minimum_tessellation
    );
    data.grid_and_options = uint4(
        grid_resolution,
        grid_resolution,
        static_cast<uint32_t>(preset_index),
        static_cast<uint32_t>(std::clamp(config.tessellated_surface_debug_mode, 0, 3))
    );

    Array<byte> upload(sizeof(TessellatedSurfaceData));
    std::memcpy(upload.data(), &data, sizeof(TessellatedSurfaceData));
    context.cmd_list.CopyFrom(std::move(upload), surface_data_buffer->GetView());

    DepthAttachment depth_attachment(context.textures.depth_linear_sampler.tex->GetView());
    depth_attachment.action = EAttachmentAction::AC_DS_LOAD_STORE_DEPTH;

    const auto render_area = context.textures.base_color.GetRect2D();
    Array<SingleDrawParam> draw_params{
        SingleDrawParam{6u, grid_resolution * grid_resolution, 0u, 0u, 0u}
    };

    context.cmd_list.PushScopeWithTimeScope("Tessellated Surface");
    context.cmd_list.Gfx(pipeline, surface_data_buffer->GetView())
        .Draw(
            "Tessellated Surface Pass",
            render_area,
            std::move(draw_params),
            depth_attachment,
            ColorAttachment{
                context.textures.base_color.tex,
                EAttachmentAction::AC_LOAD_STORE,
                float4(0.0f, 0.0f, 0.0f, 0.0f)
            },
            ColorAttachment{
                context.textures.normal.tex,
                EAttachmentAction::AC_LOAD_STORE,
                float4(0.0f, 0.0f, 0.0f, 0.0f)
            },
            ColorAttachment{
                context.textures.metal_rough_ao.tex,
                EAttachmentAction::AC_LOAD_STORE,
                float4(0.0f, 0.0f, 0.0f, 0.0f)
            }
        );
    context.cmd_list.PopScopeWithTimeScope();
}

} // namespace Moer::Render::Raster
