/**
 * 此文件应该只有在宏 WITH_CUDA 被设置的情况下使用
*/
#pragma once

#if !defined(WITH_CUDA)
#error "This header requires WITH_CUDA=1"
#endif

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class UpsamplePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(UpsamplePipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(UpsamplePipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class UpsamplePass {
public:
    UpsamplePass(RasterContext& context) {
        GfxPsoCreateInfo pso_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.upsample_output.tex->GetFormat())}
        );

        upsample_pipeline = context.manager.Raster()
                                .Vertex("core/utils/FullScreenQuad.hlsl")
                                .Pixel("pipelines/postprocess/common/Upsample.hlsl")
                                .Build<UpsamplePipeline>(std::move(pso_info));
    }

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle input_image) {
        UpsamplePipelineBindlessParam param;

        //param.low_res_tex = low_res_tex;
        param.upsample_mode = static_cast<uint32>(ui_config.upsample_mode);
        param.outSize       = ui_config.outSize_x;
        param.inSize        = ui_config.inSize_x;
        param.input_image   = input_image.hdl;
        //param.high_res_depth = context.textures.position.hdl; // 可用 position/深度图作为引导
        //param.inv_low_res = float2(1.0f / ui_config.render_res.x, 1.0f / ui_config.render_res.y);
        //param.inv_high_res = float2(1.0f / ui_config.display_res.x, 1.0f / ui_config.display_res.y);
        //param.scale_ratio = float2(
        //ui_config.display_res.x / ui_config.render_res.x,
        //ui_config.display_res.y / ui_config.render_res.y
        //);
        //param.sharpness = ui_config.upsample_sharpness;

        context.cmd_list.Gfx(upsample_pipeline, context.bdls, param)
            .Draw(
                "Upsample Pass",
                context.textures.upsample_output.GetRect2D(),
                RasterTool::GetFullScreenDrawDatas(),
                ColorAttachment(context.textures.upsample_output.tex)
            );

        return context.textures.upsample_output;
    }

private:
    UpsamplePipeline upsample_pipeline;
};
} // namespace Moer::Render::Raster
