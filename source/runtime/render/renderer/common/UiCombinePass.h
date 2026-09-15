#pragma once

// 将场景与编辑器 UI 合成到当前交换链或平台窗口的 Framebuffer。

#include "PixelFormat.h"
#include "misc/STL.h"
#include "rendergraph/RenderGraph.h"
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
    DEFINE_SHADER_SAMPLER(linear_sampler);
    DEFINE_SHADER_CONSTANT_STRUCT(Param, scene_rect);

    DEFINE_SHADER_ARGS(scene_color, linear_sampler, scene_rect);
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
    struct GraphResources {
        RenderGraph::TextureHandle processing_input{};
        RenderGraph::TextureHandle selected_framebuffer{};
        RenderGraph::TextureHandle window_framebuffer{};
        RenderGraph::TextureHandle output{};
    };

    struct RecordParameters {
        bool                     sample_to_separate_window{false};
        TextureView              target{};
        TextureView              input_color{};
        TextureView              default_output{};
        Rect2D                   render_area{};
        Sampler                  linear_sampler{SF_LINEAR, SAM_CLAMP_TO_EDGE};
        CombineUIPipeline::Param scene_rect{};

        [[nodiscard]] TextureRef Output() const {
            return default_output.GetTexture();
        }
    };

    explicit UiCombinePass(ShaderManager& manager) {
        for (const auto format : s_supported_formats) {
            GfxPsoCreateInfo combine_pso_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            combine_ui_pipelines[format] = manager.Raster()
                                               .Vertex("features/ui/CombineGuiVert.hlsl")
                                               .Pixel("features/ui/CombineGuiFrag.hlsl")
                                               .Build<CombineUIPipeline>(std::move(combine_pso_info));

            GfxPsoCreateInfo sample_tex_pso_info(
                RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(format)}
            );
            sample_texture_pipelines[format] =
                manager.Raster()
                    .Vertex("core/common/FullScreen.vert.hlsl")
                    .Pixel("core/utils/CopyTexture.frag.hlsl")
                    .Build<SampleTexturePipeline>(std::move(sample_tex_pso_info));
        }
    }

    [[nodiscard]] RecordParameters Prepare(
        bool         is_separate_window,
        const uint2& resolution,
        float2       scene_color_pos,
        float2       scene_color_resolution,
        TextureView  input_window_frame_buffer,
        TextureView  input_color_texture,
        TextureView  default_output_texture
    ) const {
        RecordParameters parameters{};
        parameters.sample_to_separate_window =
            is_separate_window && input_window_frame_buffer.GetTexture();
        parameters.target = parameters.sample_to_separate_window ?
                                input_window_frame_buffer :
                                default_output_texture;
        parameters.input_color   = input_color_texture;
        parameters.default_output = default_output_texture;
        if (parameters.sample_to_separate_window) {
            parameters.render_area = Rect2D(
                scene_color_pos.x,
                scene_color_pos.y,
                scene_color_resolution.x,
                scene_color_resolution.y
            );
        } else {
            const float2 output_resolution = float2(resolution.x, resolution.y);
            parameters.scene_rect = CombineUIPipeline::Param{
                scene_color_pos / output_resolution,
                (scene_color_pos + scene_color_resolution) / output_resolution
            };
            parameters.render_area = Rect2D(0, 0, resolution.x, resolution.y);
        }
        return parameters;
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        ScopedGpuMarker ui_composition_marker(
            cmd_list, "UI Composition", GpuMarkerPalette::Ui()
        );
        if (parameters.sample_to_separate_window) {
            assert(
                sample_texture_pipelines.contains(parameters.target.format) &&
                "Unsupported format for SampleTexturePipeline"
            );
            cmd_list
                .Gfx(
                    sample_texture_pipelines[parameters.target.format],
                    parameters.input_color,
                    parameters.linear_sampler
                )
                .Draw(
                    "SampleTexture",
                    parameters.render_area,
                    {},
                    3,
                    {SingleDrawParam(3, 1, 0, 0, 0)},
                    ColorAttachment(parameters.target.GetTexture())
                );
            return;
        }

        assert(
            combine_ui_pipelines.contains(parameters.target.format) &&
            "Unsupported format for CombineUIPipeline"
        );
        cmd_list
            .Gfx(
                combine_ui_pipelines[parameters.target.format],
                parameters.input_color,
                parameters.linear_sampler,
                parameters.scene_rect
            )
            .Draw(
                "Combine UI Pass",
                parameters.render_area,
                {},
                3,
                {SingleDrawParam(3, 1, 0, 0, 0)},
                ColorAttachment(parameters.target.GetTexture())
            );
    }

    RenderGraph::PreparedPassHandle AddToGraph(
        RenderGraph& graph,
        bool ui_enabled,
        bool writes_external_window,
        bool is_separate_window,
        uint2 resolution,
        float2 scene_color_position,
        float2 scene_color_resolution,
        TextureView window_framebuffer,
        TextureView selected_framebuffer,
        TextureView default_output,
        TextureView processing_input,
        GraphResources resources
    );

    TextureRef Process(
        CommandList& cmd_list,
        bool         is_separate_window,
        const uint2& resolution,
        float2       scene_color_pos,
        float2       scene_color_resolution,
        TextureView  input_window_frame_buffer,
        TextureView  input_color_texture,
        TextureView  default_output_texture
    ) {
        const RecordParameters parameters = Prepare(
            is_separate_window,
            resolution,
            scene_color_pos,
            scene_color_resolution,
            input_window_frame_buffer,
            input_color_texture,
            default_output_texture
        );
        Record(cmd_list, parameters);
        return parameters.Output();
    }

private:
    UnorderedMap<EPixelFormat, CombineUIPipeline>     combine_ui_pipelines;
    UnorderedMap<EPixelFormat, SampleTexturePipeline> sample_texture_pipelines;
};

} // namespace Moer::Render
