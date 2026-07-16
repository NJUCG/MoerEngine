#pragma once

// 过滤 AI 光照结果中的噪点，并写入光栅降噪输出目标。
#include "math/Function.h"
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

/** 启用双边滤波降噪时，过滤 TensorRT 输出并写入专用降噪目标。 */
class BilateralFilterDenoiserPass {
public:
    BilateralFilterDenoiserPass(RasterContext& context) :
        output_image(context.textures.denoiser_output) {
        GfxPsoCreateInfo pipeline_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(output_image.tex->GetFormat())}
        );

        pipeline = context.manager.Raster()
                       .Vertex("core/utils/FullScreenQuad.hlsl")
                       .Pixel("pipelines/postprocess/denoise/BilateralFilterDenoiser.hlsl")
                       .Build<BilateralFilterDenoiserPipeline>(std::move(pipeline_info));
    }

    TextureWithHandle Process(
        RasterContext&      context,
        const RasterConfig& raster_config,
        TextureWithHandle   input_image
    ) {
        if (raster_config.denoiser_mode == EDenoiserMode::NONE) {
            return input_image;
        }

        BilateralFilterDenoiserPipelineBindlessParam param;

        param.inv_resolution       = 1.0f / float2(context.textures.ao_output.GetSize());
        param.kernel_radius        = raster_config.denoiser_bfd_kernel_radius;
        param.spatial_sigma_square = raster_config.denoiser_bfd_spatial_sigma_square;
        param.range_sigma_square   = raster_config.denoiser_bfd_range_sigma_square;
        param.input_image          = input_image.hdl;

        context.cmd_list.Gfx(pipeline, context.bdls, param)
            .Draw(
                "BilateralFilterDenoiser Pass",
                output_image.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(output_image.tex)
            );

        return output_image;
    }

private:
    BilateralFilterDenoiserPipeline pipeline;
    TextureWithHandle&              output_image;
};

} // namespace Moer::Render::Raster
