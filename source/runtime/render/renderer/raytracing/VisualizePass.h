#ifndef MOER_RENDER_VISUALIZE_PASS_H
#define MOER_RENDER_VISUALIZE_PASS_H

#include "RTResource.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"

namespace Moer::Render::Raytracing {

class VisualizePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(VisualizePipeline);

    DEFINE_SHADER_BUFFER(param);
    DEFINE_SHADER_TEX(direct_lighting);
    DEFINE_SHADER_TEX(diffuse_lighting);
    DEFINE_SHADER_TEX(specular_lighting);
    DEFINE_SHADER_TEX(view_depth);
    DEFINE_SHADER_TEX(emission);
    DEFINE_SHADER_TEX(output);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(
        param,
        direct_lighting,
        diffuse_lighting,
        specular_lighting,
        view_depth,
        emission,
        output,
        bdls
    );
};

struct VisualizeConfig {
    float split_ratio;
    uint  visualize_mode;
    bool  b_split;
};

class VisualizePass {
public:
    VisualizePass(RenderDevice& _device, class ShaderManager& _manager);
    void Process(
        CommandList&           _cmd_list,
        RTContext&             _rt_ctx,
        const VisualizeConfig& _config,
        BindlessArrayRef       _bdls_array
    );

private:
    RenderDevice&  device;
    ShaderManager& manager;

    VisualizeParams   params;
    BufferRef         visualize_params_buffer;
    VisualizePipeline visualize_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif