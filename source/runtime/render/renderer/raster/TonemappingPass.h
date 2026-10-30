#pragma once

#include "math/Function.h"
#include "misc/Types.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "rendergraph/RenderGraph.h"

namespace Moer::Render::Raster {

class TonemappingHistogramPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TonemappingHistogramPipeline);

    DEFINE_SHADER_CONSTANT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(input_image);  // input
    DEFINE_SHADER_BUFFER(histogram); // output

    DEFINE_SHADER_ARGS(param, input_image, histogram);
};

class TonemappingExposurePipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(TonemappingExposurePipeline);

    DEFINE_SHADER_CONSTANT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_BUFFER(histogram); // input
    DEFINE_SHADER_BUFFER(exposure);  // output

    DEFINE_SHADER_ARGS(param, histogram, exposure);
};

class TonemappingPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TonemappingPipeline);

    DEFINE_SHADER_CONSTANT(TonemappingPipelineBindlessParam, param);
    DEFINE_SHADER_TEX(input_image); // input
    DEFINE_SHADER_BUFFER(exposure); // input
    DEFINE_SHADER_BUFFER(histogram);

    DEFINE_SHADER_ARGS(param, input_image, exposure, histogram);
};

/**
 * MARK: Tonemapping Pass
 * 
 * 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
 * 
 * 创建光源或者实现曝光时，请参考 光源强度单位参考表
 * - 详见 source\runtime\render\scene\light\LightComponent.cpp: LightComponent::CreateDefaultLightComponents
 */
class TonemappingPass {
public:
    struct GraphResources {
        RenderGraph::TextureHandle input{};
        RenderGraph::BufferHandle  histogram{};
        RenderGraph::BufferHandle  exposure{};
        RenderGraph::TextureHandle tonemapping_output{};
    };

    struct RecordParameters {
        TonemappingPipelineBindlessParam shader{};
        TextureWithHandle                input{};
        TextureWithHandle                output{};
        BufferRef                        histogram{};
        BufferRef                        exposure{};
        uint3                            histogram_groups{};
    };

    TonemappingPass(RasterContext& context) :
        TonemappingPass(
            context.device,
            context.manager,
            context.textures.tonemapping_output.tex->GetFormat()
        ) {}

    TonemappingPass(RenderDevice& device, ShaderManager& manager, EPixelFormat output_format) {
        GfxPsoCreateInfo pso_full_screen_info(
            RHIRasterizeInfo::Preset(), {}, {RHIColorAttachmentInfo::Preset(output_format)}
        );

        // Pipeline
        tonemapping_pipeline = manager.Raster()
                                   .Vertex("core/utils/FullScreenQuad.hlsl")
                                   .Pixel("pipelines/postprocess/color/Tonemapping.hlsl")
                                   .Build<TonemappingPipeline>(std::move(pso_full_screen_info));

        tonemapping_histogram_pipeline = manager.Compute<TonemappingHistogramPipeline>(
            "pipelines/postprocess/color/TonemappingHistogram.hlsl"
        );

        tonemapping_exposure_pipeline = manager.Compute<TonemappingExposurePipeline>(
            "pipelines/postprocess/color/TonemappingExposure.hlsl"
        );

        // Buffer
        histogram_buffer = device.CreateBuffer<uint32>(
            "raster histogram buffer",
            TONEMAPPING_HISTOGRAM_BIN_COUNT,
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER
        );
        exposure_buffer = device.CreateBuffer<uint32>(
            "raster exposure buffer",
            1,
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER |
                EBufferUsageFlags::CPU_VISIBLE
        );
    }

    [[nodiscard]] RecordParameters
    Prepare(const RasterContext& context, const RasterConfig& ui_config, TextureWithHandle input_image)
        const {
        RecordParameters parameters{};
        parameters.input     = std::move(input_image);
        parameters.output    = context.textures.tonemapping_output;
        parameters.histogram = histogram_buffer;
        parameters.exposure  = exposure_buffer;

        const uint2 input_res = parameters.input.GetSize();
        auto&       param     = parameters.shader;
        {
            auto& ae  = param.ae;
            const auto& uae = ui_config.tonemapping_ae;

            ae.enabled = uae.enabled ? 1 : 0;

            // from [x, y] to [0, 1]: v * scale + bias
            ae.log2lum_to01_scale = 1.0f / (uae.log2lum_max - uae.log2lum_min);
            ae.log2lum_to01_bias  = -uae.log2lum_min * ae.log2lum_to01_scale;
            // from [0, 1] to [x, y]: v * scale_inv + bias_inv
            ae.log2lum_to01_scale_inv = uae.log2lum_max - uae.log2lum_min;
            ae.log2lum_to01_bias_inv  = uae.log2lum_min;

            ae.histogram_low_percentile  = uae.histogram_low_percentile;
            ae.histogram_high_percentile = uae.histogram_high_percentile;

            ae.min_adapted_luminance = uae.min_adapted_luminance;
            ae.max_adapted_luminance = uae.max_adapted_luminance;

            ae.eye_adaptation_speed_up   = uae.eye_adaptation_speed_up;
            ae.eye_adaptation_speed_down = uae.eye_adaptation_speed_down;

            ae.frame_time          = context.frame_time;
            ae.diff_log2_threshold = uae.diff_log2_threshold;

            ae.debug_visualize          = uae.debug_visualize ? 1 : 0;
            ae.aces_tonemapping_enabled = uae.aces_tonemapping_enabled ? 1 : 0;
        }
        {
            param.resolution     = input_res;
            param.resolution_inv = float2(1.f / input_res.x, 1.f / input_res.y);

            // 这个参数是共用的
            param.exposure_ev = std::exp2f(ui_config.tonemapping_exposure_ev);

            param.reinhard_enabled = ui_config.tonemapping_reinhard_enabled ? 1 : 0;

            param.debug_param = ui_config.debug_param;
        }

        parameters.histogram_groups = uint3(
            (input_res.x - 1) / TONEMAPPING_HISTOGRAM_GROUP_X + 1,
            (input_res.y - 1) / TONEMAPPING_HISTOGRAM_GROUP_Y + 1,
            1
        );
        return parameters;
    }

    void AddToGraph(
        RenderGraph&         graph,
        const RasterContext& context,
        const RasterConfig&  ui_config,
        TextureWithHandle    input_image,
        GraphResources       resources
    );

    /** The same four GPU stages can be embedded in a frame graph or executed
     *  as an isolated compiled graph with explicit import/export boundaries. */
    RENDER_API RenderGraph::PassHandle
    AddRecordPasses(RenderGraph& graph, GraphResources resources, RGPreparedValue<RecordParameters> prepared);

    [[nodiscard]] const BufferRef& GetHistogramBuffer() const {
        return histogram_buffer;
    }

    [[nodiscard]] const BufferRef& GetExposureBuffer() const {
        return exposure_buffer;
    }

    void RecordClear(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list.ClearResource(parameters.histogram->GetView(), 0);
    }

    void RecordHistogram(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list
            .Compute(
                tonemapping_histogram_pipeline, parameters.shader, parameters.input.tex, parameters.histogram
            )
            .Dispatch(parameters.histogram_groups, "Tonemapping Histogram Pass");
    }

    void RecordExposure(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list
            .Compute(
                tonemapping_exposure_pipeline, parameters.shader, parameters.histogram, parameters.exposure
            )
            .Dispatch(uint3(1, 1, 1), "Tonemapping Exposure Pass");
    }

    void RecordTonemapping(CommandList& cmd_list, const RecordParameters& parameters) {
        cmd_list
            .Gfx(
                tonemapping_pipeline,
                parameters.shader,
                parameters.input.tex,
                parameters.exposure,
                parameters.histogram
            )
            .Draw(
                "Tonemapping Pass",
                parameters.output.GetRect2D(),
                std::move(RasterTool::GetFullScreenDrawDatas()),
                ColorAttachment(parameters.output.tex)
            );
    }

    void Record(CommandList& cmd_list, const RecordParameters& parameters) {
        RecordClear(cmd_list, parameters);
        RecordHistogram(cmd_list, parameters);
        RecordExposure(cmd_list, parameters);
        RecordTonemapping(cmd_list, parameters);
    }

    TextureWithHandle
    Process(RasterContext& context, const RasterConfig& ui_config, TextureWithHandle input_image) {
        const RecordParameters parameters = Prepare(context, ui_config, std::move(input_image));
        Record(context.cmd_list, parameters);
        return parameters.output;
    }

private:
    TonemappingPipeline          tonemapping_pipeline;
    TonemappingHistogramPipeline tonemapping_histogram_pipeline;
    TonemappingExposurePipeline  tonemapping_exposure_pipeline;

    BufferRef histogram_buffer;
    BufferRef exposure_buffer;
};

} // namespace Moer::Render::Raster
