#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

namespace Moer::Render::Raster {

class ProbeUpdatePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ProbeUpdatePipeline);

    DEFINE_SHADER_BUFFER(rw_probe_data);
    DEFINE_SHADER_BUFFER(rw_visibility_atlas);
    DEFINE_SHADER_BUFFER(rw_irradiance_atlas);
    DEFINE_SHADER_BUFFER(probe_scene_data);
    DEFINE_SHADER_TEX(rw_visibility_atlas_texture);
    DEFINE_SHADER_TEX(rw_irradiance_atlas_texture);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(ProbeUpdateParam, param);
    DEFINE_SHADER_ARGS(
        rw_probe_data,
        rw_visibility_atlas,
        rw_irradiance_atlas,
        probe_scene_data,
        rw_visibility_atlas_texture,
        rw_irradiance_atlas_texture,
        bdls,
        param
    );
};

class ProbeUpdateRayQueryPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ProbeUpdateRayQueryPipeline);

    DEFINE_SHADER_BUFFER(rw_probe_data);
    DEFINE_SHADER_BUFFER(rw_visibility_atlas);
    DEFINE_SHADER_BUFFER(rw_irradiance_atlas);
    DEFINE_SHADER_BUFFER(probe_scene_data);
    DEFINE_SHADER_TEX(rw_visibility_atlas_texture);
    DEFINE_SHADER_TEX(rw_irradiance_atlas_texture);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(ProbeUpdateParam, param);
    DEFINE_SHADER_ARGS(
        rw_probe_data,
        rw_visibility_atlas,
        rw_irradiance_atlas,
        probe_scene_data,
        rw_visibility_atlas_texture,
        rw_irradiance_atlas_texture,
        tlas,
        bdls,
        param
    );

    MUTATION_BOOL(PROBE_GI_USE_RAY_QUERY);
};

MUTATION_SET(ProbeUpdateRayQueryMacros, ProbeUpdateRayQueryPipeline::PROBE_GI_USE_RAY_QUERY);

class ProbeUpdatePass {
public:
    ProbeUpdatePass(RasterContext& context) {
        probe_update_pipeline =
            context.manager.Compute<ProbeUpdatePipeline>("pipelines/raster/deferred/lighting/ProbeUpdate.comp.hlsl");

        ProbeUpdateRayQueryMacros ray_query_macros{};
        ray_query_macros.SetMutation<ProbeUpdateRayQueryPipeline::PROBE_GI_USE_RAY_QUERY>(true);
        probe_update_ray_query_pipeline = context.manager.Compute<ProbeUpdateRayQueryPipeline>(
            "pipelines/raster/deferred/lighting/ProbeUpdate.comp.hlsl",
            ray_query_macros
        );
    }

    void Process(RasterContext& context, const RasterConfig& config, const Scene& scene, uint64 frame_index) {
        ProbeVolumeResource::UpdateInfo update_info = context.probe_volume.PrepareUpdate(config, scene, frame_index);

        if (!update_info.enabled || update_info.probe_count == 0) {
            return;
        }

        const uint dispatch_count =
            (update_info.probe_count + RASTER_PROBE_UPDATE_GROUP_SIZE - 1u) / RASTER_PROBE_UPDATE_GROUP_SIZE;

        context.probe_volume.UpdateSceneData(context.cmd_list, scene);

        RaytracingSceneRef rt_scene = context.rt_scene();
        if (rt_scene && rt_scene->GetTlas()) {
            update_info.param.probe_trace_config.w = 1.0f;
            context.cmd_list
                .Compute(
                    probe_update_ray_query_pipeline,
                    context.probe_volume.GetProbeBufferView(),
                    context.probe_volume.GetVisibilityAtlasBufferView(),
                    context.probe_volume.GetIrradianceAtlasBufferView(),
                    context.probe_volume.GetSceneDataBufferView(),
                    context.probe_volume.GetVisibilityAtlasTextureView(),
                    context.probe_volume.GetIrradianceAtlasTextureView(),
                    rt_scene->GetTlas(),
                    context.bdls,
                    update_info.param
                )
                .Dispatch(uint3(dispatch_count, 1, 1), "Probe GI DDGI Ray Query Update Pass");
            RasterTool::LogDebugEverySeconds("[ProbeGI] DDGI ray-query probe update active.", 3.0);
            return;
        }

        update_info.param.probe_trace_config.w = 0.0f;
        context.cmd_list
            .Compute(
                probe_update_pipeline,
                context.probe_volume.GetProbeBufferView(),
                context.probe_volume.GetVisibilityAtlasBufferView(),
                context.probe_volume.GetIrradianceAtlasBufferView(),
                context.probe_volume.GetSceneDataBufferView(),
                context.probe_volume.GetVisibilityAtlasTextureView(),
                context.probe_volume.GetIrradianceAtlasTextureView(),
                context.bdls,
                update_info.param
            )
            .Dispatch(uint3(dispatch_count, 1, 1), "Probe GI Fallback Update Pass");
        RasterTool::LogDebugEverySeconds("[ProbeGI] TLAS unavailable, using fallback probe update.", 3.0);
    }

private:
    ProbeUpdatePipeline         probe_update_pipeline;
    ProbeUpdateRayQueryPipeline probe_update_ray_query_pipeline;
};

} // namespace Moer::Render::Raster
