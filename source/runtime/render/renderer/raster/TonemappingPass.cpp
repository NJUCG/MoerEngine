#include "TonemappingPass.h"
#include "RasterTool.h"
#include "profile/ProfileScope.h"

#include <bit>
#include <cmath>

namespace Moer::Render::Raster {

TonemappingPass::TonemappingPass(RasterContext& context) :
    TonemappingPass(context.device, context.manager, context.textures.tonemapping_output.tex->GetFormat()) {}

TonemappingPass::TonemappingPass(RenderDevice& device, ShaderManager& manager, EPixelFormat output_format) {
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
    exposure_buffer = device.CreateBuffer<uint32>(
        "raster exposure buffer",
        1,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER |
            EBufferUsageFlags::CPU_VISIBLE
    );
}

TonemappingPass::RecordParameters TonemappingPass::Prepare(
    const RasterContext& context, const RasterConfig& ui_config,
    TextureWithHandle input_image, float frame_time
) const {
    RecordParameters parameters{};
    parameters.input     = std::move(input_image);
    parameters.output    = context.textures.tonemapping_output;
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

        ae.frame_time          = frame_time;
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

void TonemappingPass::AddToGraph(
    RenderGraph& graph, const RasterContext& context, const RasterConfig& config,
    TextureWithHandle input_image, float frame_time, GraphResources resources
) {
    MOER_PROFILE_SCOPE("Raster.AddToGraph.TonemappingPass");
    auto prepared = graph.AddSetupPass(
        "Tonemapping.Prepare", std::move(input_image),
        [this, &context, &config, frame_time](const TextureWithHandle& input) {
            return Prepare(context, config, input, frame_time);
        }
    );
    if (prepared.IsValid()) static_cast<void>(AddRecordPasses(graph, resources, prepared));
}

TonemappingPass::GraphResult TonemappingPass::AddRecordPasses(
    RenderGraph& graph, GraphResources resources, RGPreparedValue<RecordParameters> prepared
) {
    const auto histogram = graph.CreateTransientBuffer("Tonemapping.Histogram", RGTransientBufferDesc{
        .element_count = TONEMAPPING_HISTOGRAM_BIN_COUNT,
        .stride = sizeof(uint32),
        .usage = EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER |
                 EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC,
    });
    graph.AddRecordPass(
        "Tonemapping.ClearHistogram",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Transfer)
                .Write(histogram, RenderGraph::BufferAccessType::TransferDestination);
        },
        [&graph, histogram](CommandList& commands) {
            ScopedGpuMarker marker(commands, "Pass: Tonemapping.ClearHistogram", GpuMarkerPalette::Pass());
            RecordClear(commands, graph.GetPhysicalBuffer(histogram)->GetView());
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.AddRecordPass(
        "Tonemapping.BuildHistogram",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
                .Read(resources.input, RenderGraph::TextureAccessType::Sampled)
                .ReadWrite(histogram, RenderGraph::BufferAccessType::Storage);
        },
        [this, &graph, histogram, prepared](CommandList& commands) {
            ScopedGpuMarker marker(commands, "Pass: Tonemapping.BuildHistogram", GpuMarkerPalette::Pass());
            RecordHistogram(commands, prepared.Get(), graph.GetPhysicalBuffer(histogram)->GetView());
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    graph.AddRecordPass(
        "Tonemapping.CalculateExposure",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
                .Read(histogram, RenderGraph::BufferAccessType::ShaderRead)
                .ReadWrite(resources.exposure, RenderGraph::BufferAccessType::Storage);
        },
        [this, &graph, histogram, prepared](CommandList& commands) {
            ScopedGpuMarker marker(commands, "Pass: Tonemapping.CalculateExposure", GpuMarkerPalette::Pass());
            RecordExposure(commands, prepared.Get(), graph.GetPhysicalBuffer(histogram)->GetView());
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    const auto apply = graph.AddRecordPass(
        "Tonemapping.Apply",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics)
                .Read(resources.input, RenderGraph::TextureAccessType::Sampled)
                .Read(histogram, RenderGraph::BufferAccessType::ShaderRead)
                .Read(resources.exposure, RenderGraph::BufferAccessType::ShaderRead)
                .Write(resources.tonemapping_output, RenderGraph::TextureAccessType::ColorAttachment);
        },
        [this, &graph, histogram, prepared](CommandList& commands) {
            ScopedGpuMarker marker(commands, "Pass: Tonemapping.Apply", GpuMarkerPalette::Pass());
            RecordTonemapping(commands, prepared.Get(), graph.GetPhysicalBuffer(histogram)->GetView());
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    return {.apply = apply, .histogram = histogram};
}

void TonemappingPass::ResetExposure(CommandList& commands) {
    commands.ClearResource(exposure_buffer->GetView(), std::bit_cast<uint32>(1.0f));
}

void TonemappingPass::RecordClear(CommandList& commands, BufferView histogram) {
    commands.ClearResource(histogram, 0);
}

void TonemappingPass::RecordHistogram(
    CommandList& commands, const RecordParameters& parameters, BufferView histogram
) {
    commands.Compute(tonemapping_histogram_pipeline, parameters.shader, parameters.input.tex, histogram)
        .Dispatch(parameters.histogram_groups, "Tonemapping Histogram Pass");
}

void TonemappingPass::RecordExposure(
    CommandList& commands, const RecordParameters& parameters, BufferView histogram
) {
    commands.Compute(tonemapping_exposure_pipeline, parameters.shader, histogram, parameters.exposure)
        .Dispatch(uint3(1, 1, 1), "Tonemapping Exposure Pass");
}

void TonemappingPass::RecordTonemapping(
    CommandList& commands, const RecordParameters& parameters, BufferView histogram
) {
    commands.Gfx(tonemapping_pipeline, parameters.shader, parameters.input.tex, parameters.exposure, histogram)
        .Draw("Tonemapping Pass", parameters.output.GetRect2D(), RasterTool::GetFullScreenDrawDatas(),
            ColorAttachment(parameters.output.tex));
}

} // namespace Moer::Render::Raster
