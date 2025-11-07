#pragma once

#include "math/Function.h"
#include "scene/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class RtaoDenoiserPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(RtaoDenoiserPassPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(RtaoDenoiserPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class CopyPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(CopyPassPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(CopyPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: RtaoDenoiser Pass
 * 
 * 这个Pass主要是为了解决TensorRT Pass网络产生的噪点；
 * 注意，这个pass重用了ao_output作为输出
 */
class RtaoDenoiserPass {
public:
    RtaoDenoiserPass(RasterContext& context) :
        img_denoiser_history_write(context.textures.ao_denoiser_accumulate),
        img_ao_only(context.textures.ao_output_ambient_only) {
        {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                {RHIColorAttachmentInfo::Preset(img_denoiser_history_write.tex->GetFormat()),
                 RHIColorAttachmentInfo::Preset(context.textures.ao_output.tex->GetFormat())}
            );

            rtao_denoiser_pso = context.manager.Raster()
                                    .Vertex("utils/FullScreenQuad.hlsl")
                                    .Pixel("raster/post_process/RtaoDenoiser.hlsl")
                                    .Build<RtaoDenoiserPassPipeline>(std::move(pso_full_screen_info));
        }
        {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(img_ao_only.tex->GetFormat())}
            );
            copy_pso = context.manager.Raster()
                           .Vertex("utils/FullScreenQuad.hlsl")
                           .Pixel("raster/CopyPass.hlsl")
                           .Build<CopyPassPipeline>(std::move(pso_full_screen_info));
        }
    }

    // 注意，这个函数是原地操作
    uint ProcessInPlace(RasterContext& context, const RasterConfig& ui_config, uint ao_only_idx) {
        // AO不为RTAO或RTAO_AO_ONLY => return
        if (ui_config.ao_mode != EAoMode::RTAO && ui_config.ao_mode != EAoMode::RTAO_AO_ONLY)
            return ao_only_idx;
        // 没有开启降噪 => return
        if (!ui_config.rtao_denoiser_enable)
            return ao_only_idx;

        if (ao_only_idx == 0) {
            img_denoiser_history_read  = context.textures.ao_denoiser_accumulate_1;
            img_denoiser_history_write = context.textures.ao_denoiser_accumulate;
            img_ao_only                = context.textures.ao_output_ambient_only;
            img_ao_only_prev           = context.textures.ao_output_ambient_only_1;
        } else {
            img_denoiser_history_read  = context.textures.ao_denoiser_accumulate;
            img_denoiser_history_write = context.textures.ao_denoiser_accumulate_1;
            img_ao_only                = context.textures.ao_output_ambient_only_1;
            img_ao_only_prev           = context.textures.ao_output_ambient_only;
        }

        // Pass 1
        RtaoDenoiserPassBindlessParam param;
        param.history_ao_tex         = img_denoiser_history_read.handle;
        param.curr_ao_tex            = img_ao_only.handle;
        param.color_tex              = context.textures.lighting_output.handle;
        param.motion_vector_tex      = context.textures.camera_motion_vector.handle;
        param.depth_tex              = context.textures.depth_nearest_sampler.handle;
        param.normal_tex             = context.textures.normal.handle;
        param.history_ratio          = ui_config.rtao_denoiser_history_ratio;
        param.is_rtao_ao_only        = (ui_config.ao_mode == EAoMode::RTAO_AO_ONLY) ? 1 : 0;
        param.is_reprojection_enable = ui_config.rtao_denoiser_reprojection_enable;
        param.is_validation_enable   = ui_config.rtao_denoiser_validation_enable;

        context.cmd_list.Gfx(rtao_denoiser_pso, context.bdls, param)
            .Draw(
                "RtaoDenoiser Pass",
                img_denoiser_history_write.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(img_denoiser_history_write.tex),
                ColorAttachment(context.textures.ao_output.tex)
            );

        // Pass 2
        CopyPassBindlessParam copy_param;
        copy_param.input_image = img_denoiser_history_write.handle;

        context.cmd_list.Gfx(copy_pso, context.bdls, copy_param)
            .Draw(
                "RtaoDenoiser Copy Pass",
                img_ao_only.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(img_ao_only.tex)
            );

        // return output_image.handle;
        return ao_only_idx ^ 1; // 0 <-> 1
    }

private:
    RtaoDenoiserPassPipeline rtao_denoiser_pso;
    CopyPassPipeline         copy_pso;
    TextureWithHandle        img_denoiser_history_write;
    TextureWithHandle        img_denoiser_history_read;
    TextureWithHandle        img_ao_only;
    TextureWithHandle        img_ao_only_prev;
};

} // namespace Moer::Render::Raster