#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

namespace Moer::Render::Raster {

class ProbeUpdatePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(ProbeUpdatePipeline);

    DEFINE_SHADER_BUFFER(rw_probe_data);
    DEFINE_SHADER_CONSTANT_STRUCT(ProbeUpdateParam, param);
    DEFINE_SHADER_ARGS(rw_probe_data, param);
};

class ProbeUpdatePass {
public:
    ProbeUpdatePass(RasterContext& context) {
        probe_update_pipeline =
            context.manager.Compute<ProbeUpdatePipeline>("pipelines/raster/deferred/lighting/ProbeUpdate.comp.hlsl");
    }

    void Process(RasterContext& context, const RasterConfig& config, const Scene& scene, uint64 frame_index) {
        const ProbeVolumeResource::UpdateInfo update_info =
            context.probe_volume.PrepareUpdate(config, scene, frame_index);

        if (!update_info.enabled || update_info.probe_count == 0) {
            return;
        }

        const uint dispatch_count =
            (update_info.probe_count + RASTER_PROBE_UPDATE_GROUP_SIZE - 1u) / RASTER_PROBE_UPDATE_GROUP_SIZE;

        context.cmd_list.Compute(probe_update_pipeline, context.probe_volume.GetProbeBufferView(), update_info.param)
            .Dispatch(uint3(dispatch_count, 1, 1), "Probe GI Update Pass");
    }

private:
    ProbeUpdatePipeline probe_update_pipeline;
};

} // namespace Moer::Render::Raster
