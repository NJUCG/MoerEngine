#ifndef MOER_RENDER_SHOW_TEXTURE_PASS_H
#define MOER_RENDER_SHOW_TEXTURE_PASS_H

#include "RaytracingGraphResources.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/utils/ShaderParameters.h"

namespace Moer::Render {
class ShaderManager;
}

namespace Moer::Render::Raytracing {

class ShowTexturePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(ShowTexturePipeline);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_TEX(src_tex);
    DEFINE_SHADER_CONSTANT_STRUCT(ShowTextureParams, param);

    DEFINE_SHADER_ARGS(param, src_tex, bdls);
};

class ShowTexturePass {
public:
    struct PreparedCommand {
        ShowTextureParams params{};
    };

    struct RecordResources {
        TextureRef source{};
        TextureRef target{};
    };

    ShowTexturePass(ShaderManager& manager, BindlessArrayRef bindless_array);

    void Process(
        CommandList&             cmd_list,
        ShowTextureParams        params,
        const TextureRef&        source,
        const TextureRef&        target
    );
    bool AddPass(
        RenderGraph&               graph,
        RenderGraph::TextureHandle source_handle,
        RenderGraph::TextureHandle target_handle,
        ShowTextureParams          params,
        const TextureRef&          source,
        const TextureRef&          target,
        RenderGraph::TokenHandle   frame_setup_ready,
        RenderGraph::TokenHandle   presentation_ready
    );

private:
    struct RecordingOwner {
        BindlessArrayRef    bindless_array{};
        ShowTexturePipeline pipeline{};
    };

    static PreparedCommand Prepare(
        ShowTextureParams   params,
        const TextureRef&   source,
        const TextureRef&   target
    );
    static RecordResources CaptureResources(
        const TextureRef& source,
        const TextureRef& target
    );
    static void Record(
        CommandList&           cmd_list,
        RecordingOwner&        owner,
        const PreparedCommand& command,
        const RecordResources& resources
    );

    SharedPtr<RecordingOwner> recording_owner;
};

} // namespace Moer::Render::Raytracing

#endif
