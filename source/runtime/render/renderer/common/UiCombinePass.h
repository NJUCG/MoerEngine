#pragma once

#include "PixelFormat.h"
#include "misc/STL.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

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
    static constexpr EPixelFormat s_supported_formats[] =
        {PF_R8G8B8A8_UNORM, PF_R8G8B8A8_SRGB, PF_B8G8R8A8_UNORM, PF_B8G8R8A8_SRGB};

public:
    UiCombinePass(ShaderManager& _manager) {
        // combine_ui_pipeline = [&]() {
        //     GfxPsoCreateInfo combine_pso_info(
        //         RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(output_format)}
        //     );
        //     return _manager.Raster()
        //         .Vertex("features/ui/CombineGuiVert.hlsl")
        //         .Pixel("features/ui/CombineGuiFrag.hlsl")
        //         .Build<CombineUIPipeline>(std::move(combine_pso_info));
        // }();

        // sample_texture_pipeline = [&]() {
        //     GfxPsoCreateInfo sample_tex_pso_info(
        //         RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(output_format)}
        //     );
        //     return _manager.Raster()
        //         .Vertex("core/common/FullScreen.vert.hlsl")
        //         .Pixel("core/utils/CopyTexture.frag.hlsl")
        //         .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));
        // }();

        for (auto format : s_supported_formats) {
            GfxPsoCreateInfo combine_pso_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            combine_ui_pipelines[format] = _manager.Raster()
                                               .Vertex("features/ui/CombineGuiVert.hlsl")
                                               .Pixel("features/ui/CombineGuiFrag.hlsl")
                                               .Build<CombineUIPipeline>(std::move(combine_pso_info));

            GfxPsoCreateInfo sample_tex_pso_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            sample_texture_pipelines[format] =
                _manager.Raster()
                    .Vertex("core/common/FullScreen.vert.hlsl")
                    .Pixel("core/utils/CopyTexture.frag.hlsl")
                    .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));
        }
    }

    TextureRef Process(
        CommandList& cmd_list,
        bool         is_seperate_window,
        const uint2& resolution,
        float2       scene_color_pos,
        float2       scene_color_resolution,
        TextureView  input_window_frame_buffer,
        TextureView  input_color_texture,
        TextureView  input_ui_texture, // TODO: is this necessary?
        TextureView  default_output_texture
    ) {
        if (is_seperate_window && input_window_frame_buffer.GetTexture()) {
            assert(
                sample_texture_pipelines.contains(input_window_frame_buffer.format) &&
                "Unsupported format for SampleTexturePipeline"
            );
            auto frame_buffer = input_window_frame_buffer;
            cmd_list
                .Gfx(
                    sample_texture_pipelines[frame_buffer.format],
                    input_color_texture,
                    Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
                )
                .Draw(
                    "SampleTexture",
                    Rect2D(
                        scene_color_pos.x,
                        scene_color_pos.y,
                        scene_color_resolution.x,
                        scene_color_resolution.y
                    ),
                    {},
                    3,
                    {SingleDrawParam(3, 1, 0, 0, 0)},
                    ColorAttachment(frame_buffer.GetTexture())
                );
            if (default_output_texture.GetTexture()) {
                assert(
                    sample_texture_pipelines.contains(default_output_texture.format) &&
                    "Unsupported format for SampleTexturePipeline"
                );
                cmd_list
                    .Gfx(
                        sample_texture_pipelines[default_output_texture.format],
                        input_ui_texture,
                        Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE)
                    )
                    .Draw(
                        "CopyMainGUI",
                        Rect2D(0, 0, resolution.x, resolution.y),
                        {},
                        3,
                        {SingleDrawParam(3, 1, 0, 0, 0)},
                        ColorAttachment(default_output_texture.GetTexture())
                    );
                return default_output_texture.GetTexture();
            }
            return frame_buffer.GetTexture();
        } else {
            assert(
                combine_ui_pipelines.contains(default_output_texture.format) &&
                "Unsupported format for CombineUIPipeline"
            );
            float2 f_res  = float2(resolution.x, resolution.y);
            float2 min_xy = scene_color_pos / f_res;
            float2 max_xy = (scene_color_pos + scene_color_resolution) / f_res;
            cmd_list
                .Gfx(
                    combine_ui_pipelines[default_output_texture.format],
                    input_color_texture,
                    input_ui_texture,
                    Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE),
                    CombineUIPipeline::Param{min_xy, max_xy}
                )
                .Draw(
                    "Combine UI Pass",
                    Rect2D(0, 0, resolution.x, resolution.y),
                    {},
                    3,
                    {SingleDrawParam(3, 1, 0, 0, 0)},
                    ColorAttachment(default_output_texture.GetTexture())
                );
            return default_output_texture.GetTexture();
        }
    }

private:
    UnorderedMap<EPixelFormat, CombineUIPipeline>     combine_ui_pipelines;
    UnorderedMap<EPixelFormat, SampleTexturePipeline> sample_texture_pipelines;
};

} // namespace Moer::Render