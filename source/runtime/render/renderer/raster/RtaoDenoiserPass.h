#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
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
 * 时序降噪：读取当前帧 ao_only 和历史 accumulate，
 * 输出降噪后的 accumulate，再通过 Copy Pass 写回 ao_only。
 */
class RtaoDenoiserPass {
public:
    struct RecordParameters {
        bool                           enabled{false};
        bool                           half_resolution{false};
        BindlessArrayRef               bindless{};
        TextureWithHandle              history_write{};
        TextureWithHandle              ao_only{};
        RtaoDenoiserPassBindlessParam  denoise{};
        CopyPassBindlessParam          copy{};
    };

    RtaoDenoiserPass(RasterContext& context) :
        img_denoiser_history_write(context.textures.ao_denoiser_accumulate),
        img_ao_only(context.textures.ao_output_ambient_only) {
        {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(),
                {},
                {RHIColorAttachmentInfo::Preset(img_denoiser_history_write.tex->GetFormat())}
            );

            rtao_denoiser_pso = context.manager.Raster()
                                    .Vertex("core/utils/FullScreenQuad.hlsl")
                                    .Pixel("pipelines/postprocess/denoise/RtaoDenoiser.hlsl")
                                    .Build<RtaoDenoiserPassPipeline>(std::move(pso_full_screen_info));
        }
        {
            GfxPsoCreateInfo pso_full_screen_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(img_ao_only.tex->GetFormat())}
            );
            copy_pso = context.manager.Raster()
                           .Vertex("core/utils/FullScreenQuad.hlsl")
                           .Pixel("pipelines/postprocess/common/CopyPass.hlsl")
                           .Build<CopyPassPipeline>(std::move(pso_full_screen_info));
        }
    }

    void ResetHistory() {
        history_valid               = false;
        history_configuration_valid = false;
    }

    [[nodiscard]] RecordParameters Prepare(
        RasterContext&       context,
        const RasterConfig&  ui_config,
        uint                 ao_only_idx
    ) const {
        RecordParameters parameters{};
        parameters.enabled =
            (ui_config.ao_mode == EAoMode::RTAO ||
             ui_config.ao_mode == EAoMode::RTAO_AO_ONLY) &&
            ui_config.rtao_denoiser_enable;
        parameters.half_resolution = ui_config.ao_half_resolution;
        parameters.bindless        = context.bdls;
        if (!parameters.enabled) {
            return parameters;
        }

        const bool configuration_matches =
            history_configuration_valid &&
            history_half_resolution == parameters.half_resolution;
        TextureWithHandle history_read{};
        if (ao_only_idx == 0) {
            history_read = parameters.half_resolution ?
                               context.textures.ao_denoiser_accumulate_1_half :
                               context.textures.ao_denoiser_accumulate_1;
            parameters.history_write = parameters.half_resolution ?
                                           context.textures.ao_denoiser_accumulate_half :
                                           context.textures.ao_denoiser_accumulate;
            parameters.ao_only = parameters.half_resolution ?
                                     context.textures.ao_output_ambient_only_half :
                                     context.textures.ao_output_ambient_only;
        } else {
            history_read = parameters.half_resolution ?
                               context.textures.ao_denoiser_accumulate_half :
                               context.textures.ao_denoiser_accumulate;
            parameters.history_write = parameters.half_resolution ?
                                           context.textures.ao_denoiser_accumulate_1_half :
                                           context.textures.ao_denoiser_accumulate_1;
            parameters.ao_only = parameters.half_resolution ?
                                     context.textures.ao_output_ambient_only_1_half :
                                     context.textures.ao_output_ambient_only_1;
        }

        TextureWithHandle camera_mv_target =
            parameters.half_resolution ?
                context.textures.camera_motion_vector_half :
                context.textures.camera_motion_vector;

        auto& param = parameters.denoise;
        param.history_ao_tex    = history_read.hdl;
        param.curr_ao_tex       = parameters.ao_only.hdl;
        param.motion_vector_tex = camera_mv_target.hdl;
        param.depth_tex         = context.textures.depth_nearest_sampler.hdl;
        param.normal_tex        = context.textures.normal.hdl;
        param.inv_resolution    = float2(1.0f) / float2(parameters.ao_only.GetSize());

        param.history_ratio = configuration_matches && history_valid ?
                                  ui_config.rtao_denoiser_history_ratio :
                                  0.f;
        param.valid_depth_threshold  = ui_config.rtao_denoiser_valid_depth_threshold;
        param.valid_normal_threshold = ui_config.rtao_denoiser_valid_normal_threshold;

        param.is_reprojection_enable     = ui_config.rtao_denoiser_reprojection_enable;
        param.is_validation_enable       = ui_config.rtao_denoiser_validation_enable;
        param.is_history_clamp_enable    = ui_config.rtao_denoiser_history_clamp_enable;
        param.is_motion_weighting_enable = ui_config.rtao_denoiser_motion_weighting_enable;

        parameters.copy.input_image = parameters.history_write.hdl;
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        if (!parameters.enabled) {
            return;
        }
        cmd_list.Gfx(rtao_denoiser_pso, parameters.bindless, parameters.denoise)
            .Draw(
                "RtaoDenoiser Pass",
                parameters.history_write.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.history_write.tex)
            );

        cmd_list.Gfx(copy_pso, parameters.bindless, parameters.copy)
            .Draw(
                "RtaoDenoiser Copy Pass",
                parameters.ao_only.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.ao_only.tex)
            );
    }

    void CommitFrame(const RecordParameters& parameters) {
        history_valid               = parameters.enabled;
        history_configuration_valid = parameters.enabled;
        if (parameters.enabled) {
            history_half_resolution = parameters.half_resolution;
        }
    }

    void CommitFrame(const RasterConfig& ui_config) {
        const bool enabled =
            (ui_config.ao_mode == EAoMode::RTAO ||
             ui_config.ao_mode == EAoMode::RTAO_AO_ONLY) &&
            ui_config.rtao_denoiser_enable;
        history_valid               = enabled;
        history_configuration_valid = enabled;
        if (enabled) {
            history_half_resolution = ui_config.ao_half_resolution;
        }
    }

    uint ProcessInPlace(RasterContext& context, const RasterConfig& ui_config, uint ao_only_idx) {
        const RecordParameters parameters = Prepare(context, ui_config, ao_only_idx);
        Record(context.cmd_list, parameters);
        CommitFrame(parameters);
        return parameters.enabled ? ao_only_idx ^ 1u : ao_only_idx;
    }

private:
    RtaoDenoiserPassPipeline rtao_denoiser_pso;
    CopyPassPipeline         copy_pso;
    TextureWithHandle        img_denoiser_history_write;
    TextureWithHandle        img_ao_only;
    bool                     history_valid               = false;
    bool                     history_configuration_valid = false;
    bool                     history_half_resolution     = false;
};

} // namespace Moer::Render::Raster
