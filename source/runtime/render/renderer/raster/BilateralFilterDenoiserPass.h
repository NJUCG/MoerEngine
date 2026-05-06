#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
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
    BilateralFilterDenoiserPass(RasterContext& context) : m_output_image(context.textures.denoiser_output) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(m_output_image.tex->GetFormat())}
        );

        m_bfd_pipeline = context.manager.Raster()
                             .Vertex("core/utils/FullScreenQuad.hlsl")
                             .Pixel("pipelines/postprocess/denoise/BilateralFilterDenoiser.hlsl")
                             .Build<BilateralFilterDenoiserPipeline>(std::move(pso_full_screen_info));
    }

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle input_image) {
        if (ui_config.denoiser_mode == EDenoiserMode::NONE) {
            return input_image; // 直接返回输入
        }

        BilateralFilterDenoiserPipelineBindlessParam param;

        param.inv_resolution       = 1.0f / float2(context.textures.ao_output.GetSize());
        param.kernel_radius        = ui_config.denoiser_bfd_kernel_radius;
        param.spatial_sigma_square = ui_config.denoiser_bfd_spatial_sigma_square;
        param.range_sigma_square   = ui_config.denoiser_bfd_range_sigma_square;
        param.input_image          = input_image.hdl;

        context.cmd_list.Gfx(m_bfd_pipeline, context.bdls, param)
            .Draw(MOER_TEXT("BilateralFilterDenoiser Pass"),
                m_output_image.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(m_output_image.tex)
            );

        return m_output_image;
    }

private:
    BilateralFilterDenoiserPipeline m_bfd_pipeline;
    TextureWithHandle&              m_output_image;
};

} // namespace Moer::Render::Raster