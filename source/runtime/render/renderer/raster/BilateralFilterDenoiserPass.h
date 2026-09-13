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
    struct RecordParameters {
        bool                                           enabled{false};
        BilateralFilterDenoiserPipelineBindlessParam   shader{};
        BindlessArrayRef                               bindless{};
        TextureWithHandle                              input{};
        TextureWithHandle                              output{};
    };

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

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context,
        const RasterConfig& raster_config,
        TextureWithHandle   input_image
    ) const {
        RecordParameters parameters{};
        parameters.enabled  = raster_config.denoiser_mode != EDenoiserMode::NONE;
        parameters.bindless = context.bdls;
        parameters.input    = std::move(input_image);
        parameters.output   = parameters.enabled ? output_image : parameters.input;
        if (!parameters.enabled) {
            return parameters;
        }
        parameters.shader.inv_resolution =
            1.0f / float2(context.textures.ao_output.GetSize());
        parameters.shader.kernel_radius = raster_config.denoiser_bfd_kernel_radius;
        parameters.shader.spatial_sigma_square =
            raster_config.denoiser_bfd_spatial_sigma_square;
        parameters.shader.range_sigma_square =
            raster_config.denoiser_bfd_range_sigma_square;
        parameters.shader.input_image = parameters.input.hdl;
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        if (!parameters.enabled) {
            return;
        }
        cmd_list.Gfx(pipeline, parameters.bindless, parameters.shader)
            .Draw(
                "BilateralFilterDenoiser Pass",
                parameters.output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.output.tex)
            );
    }

    TextureWithHandle Process(
        RasterContext&      context,
        const RasterConfig& raster_config,
        TextureWithHandle   input_image
    ) {
        const RecordParameters parameters = Prepare(context, raster_config, std::move(input_image));
        Record(context.cmd_list, parameters);
        return parameters.output;
    }

private:
    BilateralFilterDenoiserPipeline pipeline;
    TextureWithHandle&              output_image;
};

} // namespace Moer::Render::Raster
