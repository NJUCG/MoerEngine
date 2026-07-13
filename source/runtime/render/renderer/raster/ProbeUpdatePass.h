#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "scene/camera/Camera.h"
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
    DEFINE_SHADER_BUFFER(probe_volume_data);
    DEFINE_SHADER_BUFFER(probe_brick_data);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(ProbeUpdateParam, param);
    DEFINE_SHADER_ARGS(
        rw_probe_data,
        rw_visibility_atlas,
        rw_irradiance_atlas,
        probe_scene_data,
        rw_visibility_atlas_texture,
        rw_irradiance_atlas_texture,
        probe_volume_data,
        probe_brick_data,
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
    DEFINE_SHADER_BUFFER(probe_volume_data);
    DEFINE_SHADER_BUFFER(probe_brick_data);
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
        probe_volume_data,
        probe_brick_data,
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

    void Process(
        RasterContext&     context,
        const RasterConfig& config,
        const Scene&        scene,
        const Camera&       camera,
        uint64              frame_index
    ) {
        ProbeVolumeResource::UpdateInfo update_info =
            context.probe_volume.PrepareUpdate(config, scene, camera.GetPosition(), frame_index);

        if (!update_info.enabled) {
            return;
        }

        context.probe_volume.UpdateSceneData(context.cmd_list, scene);

        if (update_info.job_count == 0 || update_info.scheduled_probe_count == 0) {
            return;
        }

        RaytracingSceneRef rt_scene = context.rt_scene();
        const bool rt_available = rt_scene && rt_scene->GetTlas();
        for (uint job_index = 0; job_index < update_info.job_count; ++job_index) {
            ProbeVolumeResource::UpdateJob& job = update_info.jobs[job_index];
            if (job.probe_count == 0) {
                continue;
            }

            job.param.probe_update_context.z = context.textures.cubemap_tex.hdl;

            const uint dispatch_count =
                (job.probe_count + RASTER_PROBE_UPDATE_GROUP_SIZE - 1u) / RASTER_PROBE_UPDATE_GROUP_SIZE;

            if (rt_available) {
                job.param.probe_trace_config.w = 1.0f;
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
                        context.probe_volume.GetVolumeBufferView(),
                        context.probe_volume.GetBrickBufferView(),
                        context.bdls,
                        job.param
                    )
                    .Dispatch(uint3(dispatch_count, 1, 1), "Probe GI DDGI Resident Brick Ray Query Update Pass");
                continue;
            }

            job.param.probe_trace_config.w = 0.0f;
            context.cmd_list
                .Compute(
                    probe_update_pipeline,
                    context.probe_volume.GetProbeBufferView(),
                    context.probe_volume.GetVisibilityAtlasBufferView(),
                    context.probe_volume.GetIrradianceAtlasBufferView(),
                    context.probe_volume.GetSceneDataBufferView(),
                    context.probe_volume.GetVisibilityAtlasTextureView(),
                    context.probe_volume.GetIrradianceAtlasTextureView(),
                    context.probe_volume.GetVolumeBufferView(),
                    context.probe_volume.GetBrickBufferView(),
                    context.bdls,
                    job.param
                )
                .Dispatch(uint3(dispatch_count, 1, 1), "Probe GI Fallback Resident Brick Update Pass");
        }

        if (rt_available) {
            RasterTool::LogDebugEverySeconds("[ProbeGI] DDGI ray-query resident-brick update active.", 3.0);
            return;
        }

        RasterTool::LogDebugEverySeconds("[ProbeGI] TLAS unavailable, using fallback resident-brick update.", 3.0);
    }

private:
    ProbeUpdatePipeline         probe_update_pipeline;
    ProbeUpdateRayQueryPipeline probe_update_ray_query_pipeline;
};

} // namespace Moer::Render::Raster
