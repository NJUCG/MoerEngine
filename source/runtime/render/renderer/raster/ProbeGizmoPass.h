#pragma once

#include "ProbeGeometryClassifier.h"
#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include <bit>

namespace Moer::Render::Raster {

inline uint ProbeGizmoPackFloat(float value) {
    static_assert(sizeof(uint) == sizeof(float));
    return std::bit_cast<uint>(value);
}

class ProbeGizmoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(ProbeGizmoPipeline);

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(ProbeGizmoParam, param);
    DEFINE_SHADER_ARGS(bdls, param);
};

class ProbeGizmoPass {
public:
    ProbeGizmoPass(RasterContext& context) {
        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset<Blend::ALPHA_BLEND>(context.textures.lighting_output.tex->GetFormat())},
            RHIDepthStencilStateInfo::Preset(),
            PF_UNDEFINED,
            EPrimitiveTopology::TRIANGLE_LIST
        );

        m_pipeline = context.manager.Raster()
                         .Vertex("pipelines/raster/deferred/lighting/ProbeGizmo.hlsl", "ProbeGizmoVS")
                         .Pixel("pipelines/raster/deferred/lighting/ProbeGizmo.hlsl", "ProbeGizmoPS")
                         .Build<ProbeGizmoPipeline>(std::move(pso_info));
    }

    void Process(RasterContext& context, const RasterConfig& config, const Camera& camera) {
        const uint probe_count = context.probe_volume.GetPhysicalAllocatorCapacity();
        const bool draw_probes = config.probe_gi_enabled && config.probe_gi_gizmo_enabled &&
                                 context.probe_volume.GetResidentProbeCount() != 0 &&
                                 context.probe_volume.GetBufferHandle() != 0;
        const bool draw_bounds = config.probe_gi_enabled && config.probe_gi_volume_bounds_enabled &&
                                 context.probe_volume.GetVolumeCount() != 0;
        const bool draw_adaptive_cells =
            config.probe_gi_enabled && config.probe_gi_adaptive_placement_enabled &&
            config.probe_gi_debug_mode == 9 && context.probe_volume.GetCellCount() != 0;
        if (!draw_probes && !draw_bounds && !draw_adaptive_cells) {
            return;
        }

        if (draw_probes) {
            ProbeGizmoParam param{};
            param.world2clip          = Transpose(camera.GetViewProjectionMatrix());
            param.probe_volume_config = uint4(
                RASTER_PROBE_GIZMO_DRAW_MODE_PROBES,
                static_cast<uint>(Clamp(config.probe_gi_gizmo_color_mode, 0, 3)),
                context.probe_volume.GetBufferHandle(),
                probe_count
            );
            param.gizmo_config = float4(
                Max(config.probe_gi_gizmo_size, 0.01f),
                Max(config.probe_gi_gizmo_intensity, 0.0f),
                Max(config.probe_gi_gizmo_thickness, 0.001f),
                0.0f
            );
            param.fixed_color = float4(
                config.probe_gi_gizmo_fixed_color.x,
                config.probe_gi_gizmo_fixed_color.y,
                config.probe_gi_gizmo_fixed_color.z,
                0.65f
            );
            param.camera_position = float4(camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z, 0.0f);

            RasterTool::LogDebugEverySeconds("[ProbeGI] Probe gizmo pass active.", 3.0);

            Array<SingleDrawParam> resident_draws;
            resident_draws.reserve(context.probe_volume.GetResidentBrickCount());
            for (uint brick_index = 0; brick_index < context.probe_volume.GetBrickCount(); ++brick_index) {
                const ProbeBrickGpuDesc& brick = context.probe_volume.GetBrickDesc(brick_index);
                const uint streaming_state =
                    (brick.neighbor_pages_1.w & RASTER_PROBE_STREAMING_STATE_FLAG_MASK) >>
                    RASTER_PROBE_STREAMING_STATE_FLAG_SHIFT;
                if (brick.probe_range.z == 0u || brick.probe_range.y == 0u ||
                    streaming_state != RASTER_PROBE_STREAMING_RESIDENT) {
                    continue;
                }
                resident_draws.emplace_back(
                    SingleDrawParam{18, brick.probe_range.y, 0, 0, brick.probe_range.x}
                );
            }

            context.cmd_list.Gfx(m_pipeline, context.bdls, param)
                .Draw(
                    "Probe GI Gizmo Pass",
                    context.textures.lighting_output.GetRect2D(),
                    std::move(resident_draws),
                    ColorAttachment{
                        context.textures.lighting_output.tex, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
                    }
                );
        }

        if (draw_bounds) {
            for (uint volume_index = 0; volume_index < context.probe_volume.GetVolumeCount(); ++volume_index) {
                const ProbeVolumeGpuDesc& volume = context.probe_volume.GetVolumeDesc(volume_index);
                const float3 volume_color = GetVolumeBoundsColor(config, volume_index);
                DrawBounds(
                    context,
                    camera,
                    float3(volume.origin_bias.x, volume.origin_bias.y, volume.origin_bias.z),
                    float3(volume.extent_blend.x, volume.extent_blend.y, volume.extent_blend.z),
                    volume_color,
                    Max(config.probe_gi_volume_bounds_thickness, 0.001f),
                    0.75f,
                    RASTER_PROBE_GIZMO_DRAW_MODE_BOUNDS,
                    "Probe GI Volume Bounds Pass"
                );
            }
            RasterTool::LogDebugEverySeconds("[ProbeGI] Multi-volume bounds pass active.", 3.0);
        }

        if (draw_adaptive_cells) {
            for (uint cell_index = 0u; cell_index < context.probe_volume.GetCellCount(); ++cell_index) {
                const ProbeCellGpuDesc& cell = context.probe_volume.GetCellDesc(cell_index);
                const uint volume_index = cell.coord_volume.w;
                if (volume_index >= context.probe_volume.GetVolumeCount()) {
                    continue;
                }

                const ProbeVolumeGpuDesc& volume = context.probe_volume.GetVolumeDesc(volume_index);
                const float3 volume_origin(volume.origin_bias.x, volume.origin_bias.y, volume.origin_bias.z);
                const float3 volume_extent(volume.extent_blend.x, volume.extent_blend.y, volume.extent_blend.z);
                const Box3D volume_bounds(volume_origin, volume_origin + volume_extent);
                const Box3D cell_bounds = ProbeGeometryClassifier::BuildCellInfluenceBounds(
                    float3(cell.origin_spacing.x, cell.origin_spacing.y, cell.origin_spacing.z),
                    float3(cell.extent.x, cell.extent.y, cell.extent.z),
                    float3(
                        volume.spacing_intensity.x,
                        volume.spacing_intensity.y,
                        volume.spacing_intensity.z
                    ),
                    volume_bounds
                );
                if (!cell_bounds.IsValid()) {
                    continue;
                }

                DrawBounds(
                    context,
                    camera,
                    cell_bounds.min,
                    cell_bounds.GetExtent(),
                    GetAdaptiveCellColor(cell.geometry.z),
                    Max(config.probe_gi_volume_bounds_thickness * 1.5f, 0.002f),
                    0.95f,
                    RASTER_PROBE_GIZMO_DRAW_MODE_ADAPTIVE_CELL_BOUNDS,
                    "Probe GI Adaptive Cell Bounds Pass"
                );
            }
            RasterTool::LogDebugEverySeconds("[ProbeGI] Adaptive Cell bounds pass active.", 3.0);
        }
    }

private:
    void DrawBounds(
        RasterContext& context,
        const Camera&  camera,
        float3         origin,
        float3         extent,
        float3         color,
        float          thickness,
        float          alpha,
        uint           draw_mode,
        const char*    pass_name
    ) {
        ProbeGizmoParam param{};
        param.world2clip = Transpose(camera.GetViewProjectionMatrix());
        param.probe_volume_config = uint4(
            draw_mode,
            ProbeGizmoPackFloat(origin.x),
            ProbeGizmoPackFloat(origin.y),
            ProbeGizmoPackFloat(origin.z)
        );
        param.gizmo_config = float4(
            Max(extent.x, 0.1f),
            Max(extent.y, 0.1f),
            Max(extent.z, 0.1f),
            Max(thickness, 0.001f)
        );
        param.fixed_color = float4(color.x, color.y, color.z, Clamp(alpha, 0.0f, 1.0f));
        param.camera_position =
            float4(camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z, 0.0f);

        context.cmd_list.Gfx(m_pipeline, context.bdls, param)
            .Draw(
                pass_name,
                context.textures.lighting_output.GetRect2D(),
                Array<SingleDrawParam>{SingleDrawParam{72, 1, 0, 0, 0}},
                ColorAttachment{
                    context.textures.lighting_output.tex,
                    EAttachmentAction::AC_LOAD_STORE,
                    float4(0, 0, 0, 0)
                }
            );
    }

    static float3 GetVolumeBoundsColor(const RasterConfig& config, uint volume_index) {
        if (volume_index == 0u) {
            return config.probe_gi_volume_bounds_color;
        }
        if (volume_index == 1u) {
            return float3(0.20f, 0.65f, 1.0f);
        }
        if (volume_index == 2u) {
            return float3(0.30f, 1.0f, 0.35f);
        }
        return float3(0.95f, 0.30f, 0.82f);
    }

    static float3 GetAdaptiveCellColor(uint subdivision_level) {
        if (subdivision_level == 0u) {
            return float3(0.95f, 0.10f, 0.04f);
        }
        if (subdivision_level == 1u) {
            return float3(1.0f, 0.72f, 0.05f);
        }
        return float3(0.04f, 0.32f, 1.0f);
    }

    ProbeGizmoPipeline m_pipeline;
};

} // namespace Moer::Render::Raster
