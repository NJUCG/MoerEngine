#pragma once

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
        const uint probe_count = context.probe_volume.GetProbeCount();
        const bool draw_probes = config.probe_gi_enabled && config.probe_gi_gizmo_enabled &&
                                 context.probe_volume.GetResidentProbeCount() != 0 &&
                                 context.probe_volume.GetBufferHandle() != 0;
        const bool draw_bounds = config.probe_gi_enabled && config.probe_gi_volume_bounds_enabled &&
                                 context.probe_volume.GetVolumeCount() != 0;
        if (!draw_probes && !draw_bounds) {
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
                if (brick.probe_range.z == 0u || brick.probe_range.y == 0u) {
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

                ProbeGizmoParam param{};
                param.world2clip = Transpose(camera.GetViewProjectionMatrix());
                param.probe_volume_config = uint4(
                    RASTER_PROBE_GIZMO_DRAW_MODE_BOUNDS,
                    ProbeGizmoPackFloat(volume.origin_bias.x),
                    ProbeGizmoPackFloat(volume.origin_bias.y),
                    ProbeGizmoPackFloat(volume.origin_bias.z)
                );
                param.gizmo_config = float4(
                    Max(volume.extent_blend.x, 0.1f),
                    Max(volume.extent_blend.y, 0.1f),
                    Max(volume.extent_blend.z, 0.1f),
                    Max(config.probe_gi_volume_bounds_thickness, 0.001f)
                );
                param.fixed_color = float4(volume_color.x, volume_color.y, volume_color.z, 0.75f);
                param.camera_position =
                    float4(camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z, 0.0f);

                context.cmd_list.Gfx(m_pipeline, context.bdls, param)
                    .Draw(
                        "Probe GI Volume Bounds Pass",
                        context.textures.lighting_output.GetRect2D(),
                        Array<SingleDrawParam>{SingleDrawParam{72, 1, 0, 0, 0}},
                        ColorAttachment{
                            context.textures.lighting_output.tex,
                            EAttachmentAction::AC_LOAD_STORE,
                            float4(0, 0, 0, 0)
                        }
                    );
            }
            RasterTool::LogDebugEverySeconds("[ProbeGI] Multi-volume bounds pass active.", 3.0);
        }
    }

private:
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

    ProbeGizmoPipeline m_pipeline;
};

} // namespace Moer::Render::Raster
