#pragma once

#include "misc/Types.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
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
class RENDER_API TonemappingPass {
public:
    struct GraphResources {
        RenderGraph::TextureHandle input{};
        RenderGraph::BufferHandle  exposure{};
        RenderGraph::TextureHandle tonemapping_output{};
    };

    struct RecordParameters {
        TonemappingPipelineBindlessParam shader{};
        TextureWithHandle                input{}, output{};
        BufferRef                        exposure{};
        uint3                            histogram_groups{};
    };

    struct GraphResult {
        RenderGraph::PassHandle   apply{};
        RenderGraph::BufferHandle histogram{};
    };

    explicit TonemappingPass(RasterContext& context);
    TonemappingPass(RenderDevice& device, ShaderManager& manager, EPixelFormat output_format);

    [[nodiscard]] RecordParameters Prepare(
        const RasterContext& context, const RasterConfig& config,
        TextureWithHandle input_image, float frame_time
    ) const;
    void AddToGraph(
        RenderGraph& graph, const RasterContext& context, const RasterConfig& config,
        TextureWithHandle input_image, float frame_time, GraphResources resources
    );
    // The histogram handle can extend its graph lifetime for diagnostic readback;
    // its physical allocation never escapes the declaring record callbacks.
    [[nodiscard]] GraphResult AddRecordPasses(
        RenderGraph& graph, GraphResources resources, RGPreparedValue<RecordParameters> prepared
    );

    [[nodiscard]] const BufferRef& GetExposureBuffer() const { return exposure_buffer; }
    void ResetExposure(CommandList& commands);

private:
    static void RecordClear(CommandList& commands, BufferView histogram);
    void RecordHistogram(CommandList& commands, const RecordParameters& parameters, BufferView histogram);
    void RecordExposure(CommandList& commands, const RecordParameters& parameters, BufferView histogram);
    void RecordTonemapping(CommandList& commands, const RecordParameters& parameters, BufferView histogram);

    TonemappingPipeline          tonemapping_pipeline;
    TonemappingHistogramPipeline tonemapping_histogram_pipeline;
    TonemappingExposurePipeline  tonemapping_exposure_pipeline;
    BufferRef                    exposure_buffer;
};

} // namespace Moer::Render::Raster
