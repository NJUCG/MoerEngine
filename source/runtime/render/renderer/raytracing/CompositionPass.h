#ifndef MOER_RENDER_COMPOSITION_PASS_H
#define MOER_RENDER_COMPOSITION_PASS_H

#include "RTResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

class CompositionPassPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(CompositionPassPipeline);

    DEFINE_SHADER_BUFFER(params);
    DEFINE_SHADER_TEX(out_color);
    DEFINE_SHADER_TEX(out_motion);

    DEFINE_SHADER_TEX(gbuffer_view_depth);
    DEFINE_SHADER_TEX(gbuffer_diffuse_albedo);
    DEFINE_SHADER_TEX(gbuffer_specular_roughness);
    DEFINE_SHADER_TEX(gbuffer_normal);
    DEFINE_SHADER_TEX(gbuffer_emissive);
    DEFINE_SHADER_TEX(diffuse_lighting);
    DEFINE_SHADER_TEX(specular_lighting);
    DEFINE_SHADER_TEX(denoised_diffuse_lighting);
    DEFINE_SHADER_TEX(denoised_specular_lighting);

    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(
        params,
        out_color,
        out_motion,
        gbuffer_view_depth,
        gbuffer_diffuse_albedo,
        gbuffer_specular_roughness,
        gbuffer_normal,
        gbuffer_emissive,
        diffuse_lighting,
        specular_lighting,
        denoised_diffuse_lighting,
        denoised_specular_lighting,
        bdls
    );

#pragma push_macro("WITH_NRD")            // 保存WITH_NRD宏的值
#undef WITH_NRD                           // 释放WITH_NRD宏
    MUTATION_SPARSE_UINT(WITH_NRD, 0, 1); // Value could be 0 or 1
    MUTATION_SET(MutationSet, WITH_NRD);
#pragma pop_macro("WITH_NRD") // 用之前的值重新定义WITH_NRD宏
};

class CompositionPass {
public:
    CompositionPass(
        class RenderDevice&  device,
        class ShaderManager& manager,
        BindlessArrayRef     bindless_array
    );
    void Process(class CommandList& _cmd_list, RTContext& _rt_ctx);

private:
    class RenderDevice&  device;
    class ShaderManager& manager;
    BindlessArrayRef     bindless_array;

    BufferRef            gbuffer_constants;
    CompositingConstants constants{};
    Array<byte>          upload_data;

    CompositionPassPipeline gbuffer_pass_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif
