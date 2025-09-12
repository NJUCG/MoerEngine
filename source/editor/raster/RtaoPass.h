#pragma once

#include <chrono>

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "ui/raster_ui/RasterConfig.h"

namespace Moer::Render::Raster {

class RtaoPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(RtaoPipeline);

    DEFINE_SHADER_TLAS(tlas);
    DEFINE_SHADER_CONSTANT_STRUCT(RtaoPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(tlas, bdls, param);
};

/**
 * MARK: RTAO Pass
 */
class RtaoPass {
public:
    struct RtaoPassOutput {
        uint ao_output;
        uint ambient_only_output;
    };

    RtaoPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.ao_output.tex->GetFormat()),
             RHIColorAttachmentInfo::Preset(context.textures.ao_output_ambient_only.tex->GetFormat())}
        );

        rtao_pipeline = context.manager.Raster()
                            .Vertex("utils/FullScreenQuad.hlsl")
                            .Pixel("raster/post_process/Rtao.hlsl")
                            .Build<RtaoPipeline>(std::move(pso_full_screen_info));
    }

    RtaoPassOutput Process(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const CameraRef&    camera,
        uint64              frame_idx,
        uint                input_image
    ) {
        RtaoPipelineBindlessParam param;

        param.clip2world = Transpose(camera->GetViewProjectionMatrixInv());

        param.camera_pos = camera->GetPosition();
        param.frame_idx  = frame_idx;

        param.resolution     = float2(context.resolution);
        param.inv_resolution = float2(1.0) / float2(context.resolution);

        param.input_image  = input_image;
        param.normal_tex   = context.textures.normal.handle;
        param.position_tex = context.textures.position.handle;
        param.sample_mode  = static_cast<uint>(ui_config.rtao_sample_mode);

        param.ray_trace_distance = ui_config.rtao_ray_trace_distance;

        context.cmd_list.Gfx(rtao_pipeline, context.rt_scene->GetTlas(), context.bdls, param)
            .Draw(
                "RTAO Pass",
                Rect2D(0, 0, context.resolution.x, context.resolution.y),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ao_output.tex),
                ColorAttachment(context.textures.ao_output_ambient_only.tex)
            );

        return RtaoPassOutput{
            .ao_output           = context.textures.ao_output.handle,
            .ambient_only_output = context.textures.ao_output_ambient_only.handle
        };
    }

private:
    RtaoPipeline rtao_pipeline;
};

} // namespace Moer::Render::Raster