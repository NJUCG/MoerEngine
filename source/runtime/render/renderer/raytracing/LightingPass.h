#ifndef MOER_LIGHTING_PASS_H
#define MOER_LIGHTING_PASS_H

// ReSTIR DI presampling, temporal/spatial reuse, and final sample shading.

#include "RTResource.h"
#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"

namespace Moer::Render::Raytracing {

// These pipelines intentionally share an identical argument layout so one registered argument
// set can be reused across all ReSTIR DI dispatches in a frame.
#define DI_BINDINGS()                               \
    DEFINE_SHADER_TLAS(tlas);                       \
    DEFINE_SHADER_TLAS(prev_tlas);                  \
    DEFINE_SHADER_BUFFER(resample_params);          \
    DEFINE_SHADER_BUFFER(light_reservoirs);         \
    DEFINE_SHADER_TEX(rw_diffuse_lighting);         \
    DEFINE_SHADER_TEX(rw_specular_lighting);        \
    DEFINE_SHADER_TEX(rw_temporal_sample_pos);      \
    DEFINE_SHADER_TEX(rw_gradients);                \
    DEFINE_SHADER_TEX(rw_restir_luminance);         \
    DEFINE_SHADER_TEX(rw_diffuse_lighting_prev);    \
    DEFINE_SHADER_BUFFER(rw_ris_buffer);            \
    DEFINE_SHADER_BUFFER(rw_ris_light_data_buffer); \
    DEFINE_SHADER_BUFFER(neighbor_offset_buf);      \
    DEFINE_SHADER_BINDLESS_ARRAY(bdls)

#define DI_SHADER_ARGS()                                                                                    \
    tlas, prev_tlas, resample_params, light_reservoirs, rw_diffuse_lighting, rw_specular_lighting,          \
        rw_temporal_sample_pos, rw_gradients, rw_restir_luminance, rw_diffuse_lighting_prev, rw_ris_buffer, \
        rw_ris_light_data_buffer, neighbor_offset_buf, bdls

class PresampleLightPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PresampleLightPipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class PresampleEnvMapPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PresampleEnvMapPipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class PresampleLightGridPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PresampleLightGridPipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class GenerateInitialSamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(GenerateInitialSamplePipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class TemporalResamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TemporalResamplePipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class SpatialResamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(SpatialResamplePipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class DIShadeSamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DIShadeSamplePipeline);
    DI_BINDINGS();
    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());

    // WITH_NRD is also a build macro, so temporarily release the name for the shader mutation.
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    MUTATION_SPARSE_UINT(WITH_NRD, 0, 1);
    MUTATION_SET(MutationSet, WITH_NRD);
#pragma pop_macro("WITH_NRD")
};

#undef DI_SHADER_ARGS
#undef DI_BINDINGS

class LightingPass {
public:
    LightingPass(class ShaderManager& manager, BindlessArrayRef bindless_array);

    void Process(CommandList& cmd_list, RTContext& rt_ctx);

private:
    BindlessArrayRef bindless_array;

    ResampleConstants constants;
    Array<byte>       upload_data;

    PresampleLightPipeline        presample_light_pipeline;
    PresampleEnvMapPipeline       presample_env_map_pipeline;
    PresampleLightGridPipeline    presample_light_grid_pipeline;
    GenerateInitialSamplePipeline generate_initial_sample_pipeline;
    TemporalResamplePipeline      temporal_resample_pipeline;
    SpatialResamplePipeline       spatial_resample_pipeline;
    DIShadeSamplePipeline         di_shade_sample_pipeline;

    BufferRef resample_params;
};

} // namespace Moer::Render::Raytracing

#endif
