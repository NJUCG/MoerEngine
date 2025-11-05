#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class BilateralFilterDenoiserPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(BilateralFilterDenoiserPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(BilateralFilterDenoiserPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: BilateralFilterDenoiser Pass
 * 
 * 这个Pass主要是为了解决TensorRT Pass网络产生的噪点；
 * 注意，这个pass重用了ao_output作为输出
 */
class BilateralFilterDenoiserPass {
public:
    BilateralFilterDenoiserPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.ao_output.tex->GetFormat())} // 输出给ao_output
        );

        bfd_pipeline = context.manager.Raster()
                           .Vertex("utils/FullScreenQuad.hlsl")
                           .Pixel("raster/post_process/BilateralFilterDenoiser.hlsl")
                           .Build<BilateralFilterDenoiserPipeline>(std::move(pso_full_screen_info));
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {

        BilateralFilterDenoiserPipelineBindlessParam param;

        param.inv_resolution       = 1.0f / float2(context.textures.ao_output.GetSize());
        param.kernel_radius        = ui_config.ai_bfd_kernel_radius;
        param.spatial_sigma_square = ui_config.ai_bfd_spatial_sigma_square;
        param.range_sigma_square   = ui_config.ai_bfd_range_sigma_square;
        param.input_image          = input_image;

        context.cmd_list.Gfx(bfd_pipeline, context.bdls, param)
            .Draw(
                "BiteralFilterDenoiser Pass",
                context.textures.ao_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ao_output.tex)
            );

        return context.textures.ao_output.handle;
    }

private:
    BilateralFilterDenoiserPipeline bfd_pipeline;
};

} // namespace Moer::Render::Raster