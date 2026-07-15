#ifndef MOER_PREPARE_LIGHTS_PASS_H
#define MOER_PREPARE_LIGHTS_PASS_H

#include "RTResource.h"
#include "RaytracingSceneFrameSnapshot.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

namespace Moer::Render::Raytracing {

class PrepareLightShaderPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PrepareLightShaderPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(PrepareLightsParams, param);
    DEFINE_SHADER_BUFFER(light_data);
    DEFINE_SHADER_BUFFER(light_index_mapping);
    DEFINE_SHADER_TEX(local_light_pdf);
    DEFINE_SHADER_BUFFER(prim_lights);
    DEFINE_SHADER_BUFFER(tasks);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(param, light_data, light_index_mapping, local_light_pdf, prim_lights, tasks, bdls);
};
class PrepareLightPass {
public:
    PrepareLightPass(class ShaderManager& manager, BindlessArrayRef bindless_array);
    void Process(
        class CommandList&                  cmd_list,
        RTContext&                          rt_ctx,
        const RaytracingSceneFrameSnapshot& scene_snapshot
    );

private:
    BindlessArrayRef bindless_array;

    UnorderedMap<uint64, uint> instance_light_buffer_offsets;
    UnorderedMap<uint64, uint> primitive_light_buffer_offsets;

    PrepareLightShaderPipeline prepare_light_pipeline;

    bool b_odd_frame = false;
};

} // namespace Moer::Render::Raytracing

#endif
