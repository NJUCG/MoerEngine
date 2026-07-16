#ifndef MOER_RENDER_GBUFFER_PASS_H
#define MOER_RENDER_GBUFFER_PASS_H

#include "RTResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

class RaytracingGBufferPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(RaytracingGBufferPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(GBufferPassParams, param);
    DEFINE_SHADER_BUFFER(gbuffer_constants);
    DEFINE_SHADER_TEX(gbuffer_view_depth);
    DEFINE_SHADER_TEX(gbuffer_diffuse_albedo);
    DEFINE_SHADER_TEX(gbuffer_specular_roughness);
    DEFINE_SHADER_TEX(gbuffer_normal);
    DEFINE_SHADER_TEX(gbuffer_emissive);
    DEFINE_SHADER_TEX(gbuffer_motion);
    DEFINE_SHADER_TEX(gbuffer_clip_depth);
    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(
        param,
        gbuffer_constants,
        gbuffer_view_depth,
        gbuffer_diffuse_albedo,
        gbuffer_specular_roughness,
        gbuffer_normal,
        gbuffer_emissive,
        gbuffer_motion,
        gbuffer_clip_depth,
        tlas,
        bdls
    );

    MUTATION_BOOL(PRINT_TEST);
};

MUTATION_SET(RTGBufferMacros, RaytracingGBufferPipeline::PRINT_TEST);

class PostProcessGBufferPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(PostProcessGBufferPipeline);

    DEFINE_SHADER_TEX(rw_specular_roughness);
    DEFINE_SHADER_TEX(rw_normal_roughness);
    DEFINE_SHADER_TEX(r_normal);
    DEFINE_SHADER_TEX(r_view_depth);

    DEFINE_SHADER_ARGS(rw_specular_roughness, rw_normal_roughness, r_normal, r_view_depth);

#pragma push_macro("WITH_NRD")            // 保存WITH_NRD宏的值
#undef WITH_NRD                           // 释放WITH_NRD宏
    MUTATION_SPARSE_UINT(WITH_NRD, 0, 1); // Value could be 0 or 1
    MUTATION_SET(MutationSet, WITH_NRD);
#pragma pop_macro("WITH_NRD") // 用之前的值重新定义WITH_NRD宏
};

class GBufferPass {
public:
    GBufferPass(class RenderDevice& device, class ShaderManager& manager, BindlessArrayRef bindless_array);
    void Process(class CommandList& _cmd_list, RTContext& _rt_ctx);

private:
    class RenderDevice&  device;
    class ShaderManager& manager;
    BindlessArrayRef     bindless_array;

    BufferRef        gbuffer_constants;
    GBufferConstants constants{};
    Array<byte>      upload_data;

    RaytracingGBufferPipeline  gbuffer_pass_pipeline;
    PostProcessGBufferPipeline post_process_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif
