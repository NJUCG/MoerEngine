#ifndef MOER_RENDER_GBUFFER_PASS_H
#define MOER_RENDER_GBUFFER_PASS_H

#include "RTResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer {
class Scene;
}

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
};

class GBufferPass {
public:
    GBufferPass(class RenderDevice& _device, class ShaderManager& _manager, Scene& _scene);
    void Process(class CommandList& _cmd_list, RTContext& _rt_ctx);

private:
    class RenderDevice&  device;
    class ShaderManager& manager;
    Scene&               scene;

    BufferRef        gbuffer_constants;
    GBufferConstants constants{};
    Array<byte>      upload_data;

    RaytracingGBufferPipeline  gbuffer_pass_pipeline;
    PostProcessGBufferPipeline post_process_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif