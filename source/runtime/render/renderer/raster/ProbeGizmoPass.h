#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

namespace Moer::Render::Raster {

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
        if (!config.probe_gi_enabled || !config.probe_gi_gizmo_enabled || probe_count == 0 ||
            context.probe_volume.GetBufferHandle() == 0) {
            return;
        }

        ProbeGizmoParam param{};
        param.world2clip          = Transpose(camera.GetViewProjectionMatrix());
        param.probe_volume_config = uint4(
            1u,
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

        context.cmd_list.Gfx(m_pipeline, context.bdls, param)
            .Draw(
                "Probe GI Gizmo Pass",
                context.textures.lighting_output.GetRect2D(),
                Array<SingleDrawParam>{SingleDrawParam{18, probe_count, 0, 0, 0}},
                ColorAttachment{
                    context.textures.lighting_output.tex, EAttachmentAction::AC_LOAD_STORE, float4(0, 0, 0, 0)
                }
            );
    }

private:
    ProbeGizmoPipeline m_pipeline;
};

} // namespace Moer::Render::Raster
