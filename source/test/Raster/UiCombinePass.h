#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "RasterUI.h"

namespace Moer::Render {

    class CombineUIPipeline : public RasterPipeline {

    public:
        struct Param {
            float2 min_xy;
            float2 max_xy;
        };

        DEFINE_RASTER_PIPELINE_CLASS(CombineUIPipeline);
        DEFINE_SHADER_TEX(scene_color);
        DEFINE_SHADER_TEX(gui_color);
        DEFINE_SHADER_SAMPLER(linear_sampler);
        DEFINE_SHADER_CONSTANT_STRUCT(Param, scene_rect);

        DEFINE_SHADER_ARGS(scene_color, gui_color, linear_sampler, scene_rect);
    };

    class SampleTexturePipeline : public RasterPipeline {
    public:
        DEFINE_RASTER_PIPELINE_CLASS(SampleTexturePipeline);
        DEFINE_SHADER_TEX(src_color);
        DEFINE_SHADER_SAMPLER(spl);

        DEFINE_SHADER_ARGS(src_color, spl);
    };

    class UiCombinePass {
    public:
        UiCombinePass(RasterContext& context) {
            combine_ui_pipeline = [&]() {
                GfxPsoCreateInfo combine_pso_info(
                    RHIRasterizeInfo::Preset(),
                    {},
                    {RHIColorAttachmentInfo::Preset(context.textures.output.tex->GetFormat())}
                );
                return context.manager.Raster()
                    .Vertex("CombineGuiVert.hlsl")
                    .Pixel("CombineGuiFrag.hlsl")
                    .Build<CombineUIPipeline>(std::move(combine_pso_info));
            }();

            sample_texture_pipeline = [&]() {
                GfxPsoCreateInfo sample_tex_pso_info(
                    RHIRasterizeInfo::Preset(),
                    {},
                    {RHIColorAttachmentInfo::Preset(context.textures.output.tex->GetFormat())}
                );
                return context.manager.Raster()
                    .Vertex("framework/FullScreen.vert.hlsl")
                    .Pixel("utils/CopyTexture.frag.hlsl")
                    .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));
            }();
        }

        TextureRef Process(RasterContext& context, const RasterUI::Config& ui_config, RasterUI& raster_ui) {
            TextureView input_image = raster_ui.GetSelectedFrameBuffer();

            if (raster_ui.IsSeperateWindow() && raster_ui.GetWindowFrameBuffer().GetTexture()) {
                assert(false && "Has some bug here");
                auto frame_buffer = raster_ui.GetWindowFrameBuffer();
                auto scene_res    = raster_ui.GetSceneColorResolution();
                auto scene_pos    = raster_ui.GetSceneColorPos();
                context.cmd_list
                    .Gfx(sample_texture_pipeline, input_image, Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE))
                    .Draw(
                        "SampleTexture",
                        Rect2D(scene_pos.x, scene_pos.y, scene_res.x, scene_res.y),
                        {},
                        3,
                        {SingleDrawParam(3, 1, 0, 0, 0)},
                        ColorAttachment(frame_buffer.GetTexture())
                    );
                return frame_buffer.GetTexture();
            } else {
                float2 f_res  = float2(context.resolution.x, context.resolution.y);
                float2 min_xy = raster_ui.GetSceneColorPos() / f_res;
                float2 max_xy = (raster_ui.GetSceneColorPos() + raster_ui.GetSceneColorResolution()) / f_res;
                context.cmd_list
                    .Gfx(
                        combine_ui_pipeline,
                        input_image,
                        context.textures.ui_frame_buffer.tex,
                        Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE),
                        CombineUIPipeline::Param{min_xy, max_xy}
                    )
                    .Draw(
                        "Combine UI Pass",
                        Rect2D(0, 0, context.resolution.x, context.resolution.y),
                        {},
                        3,
                        {SingleDrawParam(3, 1, 0, 0, 0)},
                        ColorAttachment(context.textures.output.tex)
                    );
                return context.textures.output.tex;
            }
        }

    private:
        CombineUIPipeline     combine_ui_pipeline;
        SampleTexturePipeline sample_texture_pipeline;
    };

} // namespace Moer::Render