#ifndef MOER_RENDER_GBUFFER_PASS_H
#define MOER_RENDER_GBUFFER_PASS_H
#include "RTResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"
namespace Moer {
    class Scene;
}
namespace Moer::Render {
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

        DEFINE_SHADER_ARGS(param,
                           gbuffer_constants,
                           gbuffer_view_depth,
                           gbuffer_diffuse_albedo,
                           gbuffer_specular_roughness,
                           gbuffer_normal,
                           gbuffer_emissive,
                           gbuffer_motion,
                           gbuffer_clip_depth,
                           tlas,
                           bdls);
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

        RaytracingGBufferPipeline gbuffer_pass_pipeline;
    };
}// namespace Moer::Render
#endif