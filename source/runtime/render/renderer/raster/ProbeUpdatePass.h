#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "rendergraph/RenderGraph.h"
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
    struct GraphResources {
        RenderGraph::TokenHandle scene{};
        RenderGraph::TokenHandle probe_volume{};
    };

    struct RecordParameters {
        bool                            enabled{false};
        bool                            ray_query{false};
        ProbeVolumeResource::UpdateInfo update{};
        ProbeVolumeResource::SceneDataUpload scene_upload{};
        BufferView                      probe_data{};
        BufferView                      visibility_atlas{};
        BufferView                      irradiance_atlas{};
        BufferView                      scene_data{};
        TextureView                     visibility_atlas_texture{};
        TextureView                     irradiance_atlas_texture{};
        BufferView                      volume_data{};
        BufferView                      brick_data{};
        RaytracingTlasRef               tlas{};
        BindlessArrayRef                bindless{};
    };

    ProbeUpdatePass(RasterContext& context) {
        probe_update_pipeline = context.manager.Compute<ProbeUpdatePipeline>(
            "pipelines/raster/deferred/lighting/ProbeUpdate.comp.hlsl"
        );

        ProbeUpdateRayQueryMacros ray_query_macros{};
        ray_query_macros.SetMutation<ProbeUpdateRayQueryPipeline::PROBE_GI_USE_RAY_QUERY>(true);
        probe_update_ray_query_pipeline =
            context.manager.Compute<ProbeUpdateRayQueryPipeline>(
                "pipelines/raster/deferred/lighting/ProbeUpdate.comp.hlsl",
                ray_query_macros
            );
    }

    [[nodiscard]] RecordParameters Prepare(
        RasterContext&      context,
        const RasterConfig& config,
        const Camera&       camera,
        uint64              frame_index
    ) {
        RecordParameters parameters{};
        parameters.update = context.probe_volume.PrepareUpdate(
            config,
            context.GetGpuSceneRes(),
            context.GetSceneUpdates(),
            camera.GetPosition(),
            frame_index
        );
        parameters.scene_upload =
            context.probe_volume.PrepareSceneDataUpload(context.GetGpuSceneRes());
        parameters.enabled                  = parameters.update.enabled;
        parameters.probe_data               = context.probe_volume.GetProbeBufferView();
        parameters.visibility_atlas         = context.probe_volume.GetVisibilityAtlasBufferView();
        parameters.irradiance_atlas         = context.probe_volume.GetIrradianceAtlasBufferView();
        parameters.scene_data               = context.probe_volume.GetSceneDataBufferView();
        parameters.visibility_atlas_texture = context.probe_volume.GetVisibilityAtlasTextureView();
        parameters.irradiance_atlas_texture = context.probe_volume.GetIrradianceAtlasTextureView();
        parameters.volume_data              = context.probe_volume.GetVolumeBufferView();
        parameters.brick_data               = context.probe_volume.GetBrickBufferView();
        parameters.bindless                 = context.bdls;

        RaytracingSceneRef rt_scene = context.rt_scene();
        parameters.ray_query = rt_scene && rt_scene->GetTlas();
        if (parameters.ray_query) {
            parameters.tlas = rt_scene->GetTlas();
        }
        for (uint job_index = 0; job_index < parameters.update.job_count; ++job_index) {
            auto& job = parameters.update.jobs[job_index];
            job.param.probe_update_context.z = context.textures.cubemap_tex.hdl;
            job.param.probe_trace_config.w   = parameters.ray_query ? 1.0f : 0.0f;
        }
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        if (!parameters.enabled) {
            return;
        }

        ProbeVolumeResource::RecordSceneDataUpload(cmd_list, parameters.scene_upload);
        if (parameters.update.job_count == 0 ||
            parameters.update.scheduled_probe_count == 0) {
            return;
        }

        for (uint job_index = 0; job_index < parameters.update.job_count; ++job_index) {
            const auto& job = parameters.update.jobs[job_index];
            if (job.probe_count == 0) {
                continue;
            }
            const uint dispatch_count =
                (job.probe_count + RASTER_PROBE_UPDATE_GROUP_SIZE - 1u) /
                RASTER_PROBE_UPDATE_GROUP_SIZE;

            if (parameters.ray_query) {
                cmd_list
                    .Compute(
                        probe_update_ray_query_pipeline,
                        parameters.probe_data,
                        parameters.visibility_atlas,
                        parameters.irradiance_atlas,
                        parameters.scene_data,
                        parameters.visibility_atlas_texture,
                        parameters.irradiance_atlas_texture,
                        parameters.tlas,
                        parameters.volume_data,
                        parameters.brick_data,
                        parameters.bindless,
                        job.param
                    )
                    .Dispatch(
                        uint3(dispatch_count, 1, 1),
                        "Probe GI DDGI Resident Brick Ray Query Update Pass"
                    );
                continue;
            }

            cmd_list
                .Compute(
                    probe_update_pipeline,
                    parameters.probe_data,
                    parameters.visibility_atlas,
                    parameters.irradiance_atlas,
                    parameters.scene_data,
                    parameters.visibility_atlas_texture,
                    parameters.irradiance_atlas_texture,
                    parameters.volume_data,
                    parameters.brick_data,
                    parameters.bindless,
                    job.param
                )
                .Dispatch(
                    uint3(dispatch_count, 1, 1),
                    "Probe GI Fallback Resident Brick Update Pass"
                );
        }
    }

    [[nodiscard]] RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        RasterContext& context,
        const RasterConfig& config,
        const Camera& camera,
        uint64 frame_index,
        GraphResources resources,
        std::span<const RenderGraph::SetupPassHandle> setup_dependencies = {}
    );

    void Process(
        RasterContext&      context,
        const RasterConfig& config,
        const Camera&       camera,
        uint64              frame_index
    ) {
        const RecordParameters parameters =
            Prepare(context, config, camera, frame_index);
        Record(context.cmd_list, parameters);
    }

private:
    ProbeUpdatePipeline         probe_update_pipeline;
    ProbeUpdateRayQueryPipeline probe_update_ray_query_pipeline;
};

} // namespace Moer::Render::Raster
