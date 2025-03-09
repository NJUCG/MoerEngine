#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "RasterUI.h"

namespace Moer::Render {

    class AoPipeline : public RasterPipeline {
    public:
        DEFINE_RASTER_PIPELINE_CLASS(AoPipeline);
        DEFINE_SHADER_CONSTANT_STRUCT(AoPipelineBindlessParam, param);
        DEFINE_SHADER_BINDLESS_ARRAY(bdls);
        DEFINE_SHADER_ARGS(bdls, param);
    };

    /**
     * MARK: AO Pass
     * 
     * TODO: SSDO Support
     */
    class AoPass {
    public:
        AoPass(RasterContext& context) {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                {RHIColorAttachmentInfo::Preset(context.textures.ao_output.tex->GetFormat())}
            );

            ao_pipeline = context.manager.Raster()
                              .Vertex("raster/post_process/PostProcessFullScreenQuad.hlsl")
                              .Pixel("raster/post_process/Ao.hlsl")
                              .Build<AoPipeline>(std::move(pso_full_screen_info));
        }

        uint Process(RasterContext& context, const RasterUI::Config& ui_config, uint input_image) {
            if (ui_config.ao_mode == 0) { return input_image; }

            AoPipelineBindlessParam param;
            param.inv_resolution    = float2(1.0f) / float2(context.resolution);
            param.ssao_intensity    = ui_config.ssao_intensity;
            param.ssao_max_distance = ui_config.ssao_max_distance;
            param.ssao_sample_count = ui_config.ssao_sample_count;
            param.ssao_radius       = ui_config.ssao_radius;
            param.ao_mode           = ui_config.ao_mode;
            param.input_image       = input_image;
            param.normal_tex        = context.textures.normal.handle;
            param.position_tex      = context.textures.normal.handle;
            param.noise_tex         = context.noise_tex.handle;

            context.cmd_list.Gfx(ao_pipeline, context.bdls, param)
                .Draw(
                    "AO Pass",
                    Rect2D(0, 0, context.resolution.x, context.resolution.y),
                    std::move(RasterTool::GetFullScreenDrawDatas()),
                    ColorAttachment(context.textures.ao_output.tex)
                );

            return context.textures.ao_output.handle;
        }

    private:
        AoPipeline ao_pipeline;
    };

} // namespace Moer::Render