#pragma once

#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class DofPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(DofPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(DofPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class DofPass {
public:
    DofPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.dof_output.tex->GetFormat())}
        );

        m_dof_pipeline = context.manager.Raster()
                             .Vertex("core/utils/FullScreenQuad.hlsl")
                             .Pixel("pipelines/postprocess/color/Dof.hlsl")
                             .Build<DofPipeline>(std::move(pso_full_screen_info));
    }

    TextureWithHandle Process(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        TextureWithHandle   input_image
    ) {

        DofPipelineBindlessParam param;

        float2 resolution = float2(input_image.GetSize());

        param.resolution      = resolution;                                     // 分辨率
        param.resolution_inv  = float2(1.f / resolution.x, 1.f / resolution.y); // 分辨率倒数
        param.input_color_tex = input_image.hdl;                                // 输入颜色纹理
        param.debug_param     = ui_config.dof_debug_param;                      // 调试参数

        context.cmd_list.Gfx(m_dof_pipeline, context.bdls, param)
            .Draw(
                "Dof Pass",
                context.textures.dof_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.dof_output.tex)
            );

        return context.textures.dof_output;
    }

private:
    DofPipeline m_dof_pipeline;
};

} // namespace Moer::Render::Raster