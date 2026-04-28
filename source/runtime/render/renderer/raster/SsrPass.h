#pragma once

#include "math/Function.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class SsrPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SsrPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SsrPipelineBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

/**
 * MARK: SSR Pass
 * 
 * GUI Help:
 *   You can set `Roughness Threshold` and `Metallic Threshold` to control which material will be reflected.
 *   If you want all materials to be reflected, set `Roughness Threshold` to 0.0 and `Metallic Threshold` to 1.0.
 *   `Force Ground Enable SSR` will force the ground to be reflected, which is useful for testing.
 *   TODO: move the above text to gui
 * 
 * TODO: use HiZ buffer to accelerate SSR (now, a simple heuristic and binary search is used for SSR)
 * TODO: glossy ssr
 * TODO: performance optimization
 * TODO: fix some artifacts (jitter)
 */
class SsrPass {
public:
    SsrPass(RasterContext& context) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(),
            {},
            {RHIColorAttachmentInfo::Preset(context.textures.ssr_output.tex->GetFormat())}
        );

        ssr_pipeline = context.manager.Raster()
                           .Vertex("core/utils/FullScreenQuad.hlsl")
                           .Pixel("pipelines/postprocess/lighting_effects/Ssr.hlsl")
                           .Build<SsrPipeline>(std::move(pso_full_screen_info));
    }

    TextureWithHandle Process(
        RasterContext&      context,
        const RasterConfig& ui_config,
        const Camera&       camera,
        TextureWithHandle   input_image
    ) {
        if (ui_config.ssr_is_ssr_enabled == 0) {
            return input_image;
        }

        SsrPipelineBindlessParam param;
        param.clip2world                     = Transpose(camera.GetViewProjectionMatrixInv());
        param.world2clip                     = Transpose(camera.GetViewProjectionMatrix());
        param.camera_position                = camera.GetPosition();
        param.near_clip                      = camera.GetNearClip();
        param.resolution                     = float2(context.textures.ssr_output.GetSize());
        param.far_clip                       = camera.GetFarClip();
        param.ssr_roughness_threshold        = ui_config.ssr_roughness_threshold;
        param.ssr_metallic_threshold         = ui_config.ssr_metallic_threshold;
        param.ssr_step_base                  = ui_config.ssr_step_base;
        param.ssr_sample_count               = ui_config.ssr_sample_count;
        param.ssr_is_enable_jitter           = ui_config.ssr_is_enable_jitter;
        param.ssr_is_force_ground_enable_ssr = ui_config.ssr_is_force_ground_enable_ssr;
        param.color_tex                      = input_image.hdl;
        param.normal_tex                     = context.textures.normal.hdl;
        param.depth_tex                      = context.textures.depth_linear_sampler.hdl;
        param.gbuffer_metal_rough_ao         = context.textures.metal_rough_ao.hdl;

        context.cmd_list.Gfx(ssr_pipeline, context.bdls, param)
            .Draw(
                "SSR Pass",
                context.textures.ssr_output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(context.textures.ssr_output.tex)
            );

        return context.textures.ssr_output;
    }

private:
    SsrPipeline ssr_pipeline;
};

} // namespace Moer::Render::Raster