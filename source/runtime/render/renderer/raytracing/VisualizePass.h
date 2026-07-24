#ifndef MOER_RENDER_VISUALIZE_PASS_H
#define MOER_RENDER_VISUALIZE_PASS_H

// 选择并显示 Raytracing 中间缓冲区，用于编辑器诊断。

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
        TextureRef ldr_color{};
        TextureRef diffuse_lighting{};
        TextureRef specular_lighting{};
        TextureRef view_depth{};
        TextureRef clip_depth{};
        TextureRef emission{};
        TextureRef normal{};
        TextureRef specular_roughness{};
        TextureRef motion{};
        TextureRef normal_roughness{};
        TextureRef debug_color{};
    };

    VisualizePass(RenderDevice& device, class ShaderManager& manager);
    void Process(
        CommandList&           cmd_list,
        RTContext&             rt_ctx,
        const VisualizeConfig& config
    );
    bool AddPasses(
        RenderGraph&                 graph,
        const RTGraphFrameResources& graph_resources,
        const RTContext&             rt_ctx,
        const VisualizeConfig&       config
    );

private:
    struct RecordingOwner {
        BufferRef         constants{};
        VisualizePipeline pipeline{};
    };

    PreparedCommand Prepare(
        const RTContext&       rt_ctx,
        const VisualizeConfig& config
    ) const;
    static RecordResources CaptureResources(const RTContext& rt_ctx);
    static void RecordConstantsUpload(
        CommandList&           cmd_list,
        const RecordingOwner&  owner,
        const PreparedCommand& command
    );
    static void RecordVisualize(
        CommandList&           cmd_list,
        RecordingOwner&        owner,
        const PreparedCommand& command,
        const RecordResources& resources
    );

    SharedPtr<RecordingOwner> recording_owner;
};

} // namespace Moer::Render::Raytracing

#endif
