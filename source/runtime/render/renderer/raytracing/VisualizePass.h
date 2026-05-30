#ifndef MOER_RENDER_VISUALIZE_PASS_H
#define MOER_RENDER_VISUALIZE_PASS_H

#include "RaytracingGraphResources.h"
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
    DEFINE_SHADER_TEX(clip_depth);
    DEFINE_SHADER_TEX(emission);
    DEFINE_SHADER_TEX(normal);
    DEFINE_SHADER_TEX(specular_roughness);
    DEFINE_SHADER_TEX(motion);
    DEFINE_SHADER_TEX(normal_roughness);
    DEFINE_SHADER_TEX(prev_view_depth);
    DEFINE_SHADER_TEX(output);

    DEFINE_SHADER_ARGS(
        param,
        direct_lighting,
        diffuse_lighting,
        specular_lighting,
        view_depth,
        clip_depth,
        emission,
        normal,
        specular_roughness,
        motion,
        normal_roughness,
        prev_view_depth,
        output
    );
};

struct VisualizeConfig {
    float split_ratio;
    uint  visualize_mode;
    bool  b_split;
};

class VisualizePass {
public:
    struct PreparedCommand {
        VisualizeParams params{};
        uint3           dispatch_groups{};
    };

    struct RecordResources {
        TextureRef       ldr_color;
        TextureRef       diffuse_lighting;
        TextureRef       specular_lighting;
        TextureRef       view_depth;
        TextureRef       clip_depth;
        TextureRef       emission;
        TextureRef       normal;
        TextureRef       specular_roughness;
        TextureRef       motion;
        TextureRef       normal_roughness;
        TextureRef       prev_view_depth;
        TextureRef       debug_color;
    };

    VisualizePass(RenderDevice& _device, class ShaderManager& _manager);
    void AddPass(
        RenderGraph&                 _graph,
        const RTGraphFrameResources& _rg,
        const RTContext&             _ctx,
        const VisualizeConfig&       _config
    );

private:
    PreparedCommand Prepare(const RTContext& _ctx, const VisualizeConfig& _config) const;
    static RecordResources CaptureResources(const RTContext& _ctx);
    void RecordConstantsUpload(CommandList& _cmd_list, const PreparedCommand& _command);
    void RecordVisualize(CommandList& _cmd_list, const PreparedCommand& _command, const RecordResources& _resources);

    RenderDevice&  device;
    ShaderManager& manager;

    BufferRef         visualize_params_buffer;
    VisualizePipeline visualize_pipeline;
};

} // namespace Moer::Render::Raytracing

#endif
