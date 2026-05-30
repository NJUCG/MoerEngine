#ifndef MOER_LIGHTING_PASS_H
#define MOER_LIGHTING_PASS_H
#include "RaytracingGraphResources.h"
#include "rhi/RHI.h"
#include "shader/ShaderPipeline.h"
namespace Moer {
namespace Render::Raytracing {

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

class TemporalResmaplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TemporalResmaplePipeline);

    DI_BINDINGS();

    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class SpatialResamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(SpatialResamplePipeline);

    DI_BINDINGS();

    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());
};

class FusedResamplingPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(FusedResamplingPipeline);
};

class DIShadeSamplePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DIShadeSamplePipeline);

    DI_BINDINGS();

    DEFINE_SHADER_ARGS(DI_SHADER_ARGS());

#pragma push_macro("WITH_NRD")            // 保存WITH_NRD宏的值
#undef WITH_NRD                           // 释放WITH_NRD宏
    MUTATION_SPARSE_UINT(WITH_NRD, 0, 1); // Value could be 0 or 1
    MUTATION_SET(MutationSet, WITH_NRD);
#pragma pop_macro("WITH_NRD") // 用之前的值重新定义WITH_NRD宏
};

class LightingPass {
public:
    struct PreparedCommand {
        ResampleConstants constants{};
        bool              has_local_lights = false;
        bool              has_env_lights   = false;
        uint3             presample_light_dispatch{};
        uint3             presample_env_dispatch{};
        uint3             presample_grid_dispatch{};
        uint3             screen_dispatch{};
    };

    struct RecordResources {
        RaytracingTlasRef  tlas;
        RaytracingTlasRef  prev_tlas;
        BufferRef          light_reservoir_buf;
        TextureRef         diffuse_lighting;
        TextureRef         specular_lighting;
        TextureRef         temporal_sample_pos;
        TextureRef         gradients;
        TextureRef         restir_luminance;
        TextureRef         prev_diffuse_lighting;
        BufferRef          ris_buf;
        BufferRef          ris_light_data_buf;
        BufferRef          neighbor_offset_buf;
        BindlessArrayRef   bindless_array;
    };

    LightingPass(class ShaderManager& _manager);

    void AddPasses(RenderGraph& _graph, const RTGraphFrameResources& _rg, const RTContext& _rt_ctx);

private:
    enum class ELightingDispatch : uint8_t;

    PreparedCommand Prepare(const RTContext& _rt_ctx) const;
    static RecordResources CaptureResources(const RTContext& _rt_ctx);
    void RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command);
    void RecordDispatch(
        CommandList&           _cmd_list,
        const PreparedCommand& _command,
        const RecordResources& _resources,
        ELightingDispatch      _dispatch
    );

    PresampleLightPipeline        presample_light_pipeline;
    PresampleEnvMapPipeline       presample_env_map_pipeline;
    PresampleLightGridPipeline    presample_light_grid_pipeline;
    GenerateInitialSamplePipeline generate_initial_sample_pipeline;
    TemporalResmaplePipeline      temporal_resmaple_pipeline;
    SpatialResamplePipeline       spatial_resample_pipeline;

    FusedResamplingPipeline fused_resampling_pipeline;
    DIShadeSamplePipeline   di_shade_sample_pipeline;

    BufferRef resample_params;
};

} // namespace Render::Raytracing

} // namespace Moer

#endif