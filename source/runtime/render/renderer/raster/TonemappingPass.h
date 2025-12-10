#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class TonemappingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TonemappingPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: Tonemap Pass
 * 
 * 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
 */
class TonemappingPass {
public:
    TonemappingPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.tonemapping_output.tex->GetFormat())}
        );

        tonemapping_pipeline = context.manager.Raster()
                                   .Vertex("core/utils/FullScreenQuad.hlsl")
                                   .Pixel("pipelines/raster/deferred/postprocess/Tonemapping.hlsl")
                                   .Build<TonemappingPipeline>(std::move(pso_full_screen_info));
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {

        TonemappingPipelineBindlessParam param;
        param.input_image = input_image;

        context.cmd_list.Gfx(tonemapping_pipeline, context.bdls, param)
            .Draw(
                "Tonemapping Pass",
                context.textures.tonemapping_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.tonemapping_output.tex)
            );

        return context.textures.tonemapping_output.handle;
    }

private:
    TonemappingPipeline tonemapping_pipeline;
};

} // namespace Moer::Render::Raster