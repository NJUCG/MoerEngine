#pragma once

#include "math/Function.h"
#include "misc/Types.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "AoFrameResources.h"
#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "rendergraph/RenderGraph.h"

namespace Moer::Render::Raster {

class RtaoDenoiserPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(RtaoDenoiserPassPipeline);
    DEFINE_SHADER_CONSTANT(RtaoDenoiserPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

// Reads noisy AO and the last accepted final output, then writes this frame's
// final output directly. AoPass owns temporal acceptance and resource selection.
class RtaoDenoiserPass {
public:
    struct GraphResources {
        RenderGraph::TextureHandle camera_motion_vector{};
        RenderGraph::TextureHandle ao_only{};
        RenderGraph::TextureHandle history_read{};
        RenderGraph::TextureHandle history_write{};
        RenderGraph::TextureHandle current_geometry{};
        RenderGraph::TextureHandle history_geometry{};
    };

    struct RecordParameters {
        BindlessArrayRef              bindless{};
        TextureWithHandle             output{};
        RtaoDenoiserPassBindlessParam denoise{};
    };

    RtaoDenoiserPass(RasterContext& context) {
        GfxPsoCreateInfo info(
            RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(context.textures.ao_output_ambient_only.tex->GetFormat())}
        );
        rtao_denoiser_pso = context.manager.Raster()
                                .Vertex("core/utils/FullScreenQuad.hlsl")
                                .Pixel("pipelines/postprocess/denoise/RtaoDenoiser.hlsl")
                                .Build<RtaoDenoiserPassPipeline>(std::move(info));
    }

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext&    context,
        const RasterConfig&     config,
        const Camera&           camera,
        const AoFrameResources& frame
    ) const {
        RecordParameters parameters{};
        parameters.bindless              = context.bdls;
        parameters.output                = frame.Output();
        auto& param                      = parameters.denoise;
        param.history_ao_tex             = frame.PreviousOutput().hdl;
        param.curr_ao_tex                = frame.EvaluationOutput().hdl;
        param.motion_vector_tex          = frame.camera_motion.hdl;
        param.current_geometry_tex       = frame.CurrentGeometry().hdl;
        param.history_geometry_tex       = frame.PreviousGeometry().hdl;
        param.camera_position            = camera.GetPosition();
        param.inv_resolution             = float2(1.0f) / float2(frame.Output().GetSize());
        param.history_ratio              = frame.has_denoised_history ? config.rtao_denoiser_history_ratio : 0.f;
        param.valid_position_threshold   = config.rtao_denoiser_valid_position_threshold;
        param.valid_normal_threshold     = config.rtao_denoiser_valid_normal_threshold;
        param.is_reprojection_enable     = config.rtao_denoiser_reprojection_enable;
        param.is_validation_enable       = config.rtao_denoiser_validation_enable;
        param.is_history_clamp_enable    = config.rtao_denoiser_history_clamp_enable;
        param.is_motion_weighting_enable = config.rtao_denoiser_motion_weighting_enable;
        return parameters;
    }

    void RecordDenoise(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list.Gfx(rtao_denoiser_pso, parameters.bindless, parameters.denoise)
            .Draw(
                "RtaoDenoiser Pass",
                parameters.output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.output.tex)
            );
    }

    void AddToGraph(
        RenderGraph&        graph,
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        AoFrameResources    frame,
        GraphResources      resources
    );

private:
    RtaoDenoiserPassPipeline rtao_denoiser_pso;
};

} // namespace Moer::Render::Raster
