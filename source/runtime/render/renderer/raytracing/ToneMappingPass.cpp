#include "ToneMappingPass.h"

// 实现基于直方图的自动曝光，并执行全屏 Tone Mapping。

#include "PixelFormat.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Moer::Render::Raytracing {

namespace {

constexpr float g_min_log_luminance = -10.f;
constexpr float g_max_log_luminance = 4.f;

float FiniteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

RenderGraph::TextureState PreferredReadState(const TextureRef& texture) {
    const auto usage = texture->GetUsage();
    const bool supports_uav =
        (usage & ETextureUsageFlags::UNORDERED_ACCESS) ==
        ETextureUsageFlags::UNORDERED_ACCESS;
    return supports_uav ?
               RenderGraph::TextureState::ShaderResource :
               RenderGraph::TextureState::Sampled;
}

} // namespace

ToneMappingPass::ToneMappingPass(
    RenderDevice&  device,
    ShaderManager& manager,
    CreateInfo     info
) {
    auto owner = MakeShared<RecordingOwner>();
    owner->histogram_pipeline =
        manager.Compute<HistogramPipeline>(
            "pipelines/raytracing/postprocess/Histogram.hlsl"
        );
    owner->exposure_pipeline =
        manager.Compute<ExposurePipeline>(
            "pipelines/raytracing/postprocess/Exposure.hlsl"
        );

    VertexStream     stream{};
    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        stream,
        {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
    );
    owner->tone_mapping =
        manager.Raster()
            .Vertex("core/utils/FullScreenQuad.hlsl")
            .Pixel("pipelines/raytracing/postprocess/ToneMappingPass.hlsl")
            .Build<ToneMappingPassPipeline>(std::move(pso_info));

    owner->constants = device.CreateBuffer<Moer::byte>(
        "tone_mapping_constants",
        sizeof(ToneMappingParams),
        EBufferUsageFlags::CONSTANT_BUFFER
    );
    owner->histogram = device.CreateBuffer<uint>(
        "histogram_buffer",
        std::max(info.histogram_bins, uint(HISTOGRAM_BINS)),
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TEXTURE_BUFFER
    );
    owner->histogram->SetName("histogram_buffer");
    owner->exposure = device.CreateBuffer<uint>(
        "exposure_buffer",
        1,
        EBufferUsageFlags::UNORDERED_ACCESS |
            EBufferUsageFlags::TEXTURE_BUFFER
    );
    recording_owner = std::move(owner);

    color_lut = std::move(info.color_lut);
    if (color_lut) {
        const uint height = color_lut->GetHeight();
        if (height > 1 && height * height == color_lut->GetWidth()) {
            color_lut_size = height;
        }
    }
}

ToneMappingPass::PreparedCommand ToneMappingPass::Prepare(
    Params            params,
    const TextureRef& source_texture
) const {
    PreparedCommand command{};
    command.params.log_luminance_scale =
        1.f / (g_max_log_luminance - g_min_log_luminance);
    command.params.log_luminance_bias =
        -g_min_log_luminance * command.params.log_luminance_scale;
    command.params.log_luminance_scale_exposure =
        g_max_log_luminance - g_min_log_luminance;
    command.params.log_luminance_bias_exposure = g_min_log_luminance;

    command.params.histogram_low_percentile = std::clamp(
        FiniteOr(params.histogram_low_percentile, 0.8f),
        0.f,
        0.99f
    );
    command.params.histogram_high_percentile = std::clamp(
        FiniteOr(params.histogram_high_percentile, 0.95f),
        command.params.histogram_low_percentile,
        1.f
    );
    command.params.eye_adaptation_speed_up =
        std::max(FiniteOr(params.eye_adaptation_speed_up, 1.f), 0.f);
    command.params.eye_adaptation_speed_down =
        std::max(FiniteOr(params.eye_adaptation_speed_down, 0.5f), 0.f);
    command.params.min_adapted_luminance =
        std::max(FiniteOr(params.min_adapted_luminance, 0.02f), 1e-6f);
    command.params.max_adapted_luminance = std::max(
        FiniteOr(params.max_adapted_luminance, 0.5f),
        command.params.min_adapted_luminance
    );
    command.params.frame_time =
        std::max(FiniteOr(frame_time, 0.f), 0.f);
    command.params.view_origin = uint2(0, 0);
    command.params.view_size = uint2(
        source_texture->GetExtent().x,
        source_texture->GetExtent().y
    );
    command.params.exposure_scale = std::exp2f(
        std::clamp(FiniteOr(params.exposure_bias, -0.5f), -32.f, 32.f)
    );
    const float white_point =
        std::max(FiniteOr(params.white_point, 3.f), 1e-4f);
    command.params.white_point_inv_squared =
        1.f / (white_point * white_point);
    command.params.source_slice = 0;

    const bool enable_color_lut =
        params.enable_color_lut && color_lut_size > 1;
    command.params.color_lut_size =
        enable_color_lut ?
            float2(color_lut_size * color_lut_size, color_lut_size) :
            float2(0.f);
    command.params.color_lut_size_inv =
        enable_color_lut ?
            float2(
                1.f / (color_lut_size * color_lut_size),
                1.f / color_lut_size
            ) :
            float2(0.f);
    command.params.frame_idx = frame_idx;
    command.params.enabled = params.enable_tone_mapping ? 1 : 0;

    command.histogram_dispatch_groups = uint3(
        (source_texture->GetExtent().x + 15) / 16,
        (source_texture->GetExtent().y + 15) / 16,
        1
    );
    command.reset_exposure =
        !initialized ||
        tone_mapping_enabled != params.enable_tone_mapping;
    return command;
}

ToneMappingPass::RecordResources ToneMappingPass::CaptureResources(
    TextureRef source_texture,
    TextureRef target_texture
) const {
    TextureRef lut = color_lut ? color_lut : source_texture;
    return RecordResources{
        .source    = std::move(source_texture),
        .target    = std::move(target_texture),
        .color_lut = std::move(lut)
    };
}

void ToneMappingPass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const RecordingOwner&  owner,
    const PreparedCommand& command
) {
    Array<byte> upload_data(sizeof(ToneMappingParams));
    std::memcpy(
        upload_data.data(),
        &command.params,
        sizeof(ToneMappingParams)
    );
    cmd_list.CopyFrom(std::move(upload_data), owner.constants->GetView());
}

void ToneMappingPass::RecordResetHistogram(
    CommandList&          cmd_list,
    const RecordingOwner& owner
) {
    cmd_list.ClearResource(owner.histogram->GetView(), 0);
}

void ToneMappingPass::RecordResetExposure(
    CommandList&          cmd_list,
    const RecordingOwner& owner
) {
    cmd_list.ClearResource(owner.exposure->GetView(), 0);
}

void ToneMappingPass::RecordHistogram(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Compute(
            owner.histogram_pipeline,
            owner.constants,
            resources.source,
            owner.histogram
        )
        .Dispatch(
            command.histogram_dispatch_groups,
            "Calculate Histogram"
        );
}

void ToneMappingPass::RecordExposure(
    CommandList&    cmd_list,
    RecordingOwner& owner
) {
    cmd_list
        .Compute(
            owner.exposure_pipeline,
            owner.constants,
            owner.histogram,
            owner.exposure
        )
        .Dispatch(uint3(1, 1, 1), "Calculate Exposure");
}

void ToneMappingPass::RecordRender(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const RecordResources& resources
) {
    Array<SingleDrawParam> full_screen_draw_data{
        SingleDrawParam{3, 1, 0, 0, 0}
    };
    Sampler linear_clamp_sampler{
        ESamplerFilter::SF_LINEAR,
        ESamplerAddressMode::SAM_CLAMP_TO_EDGE
    };
    cmd_list
        .Gfx(
            owner.tone_mapping,
            owner.constants,
            resources.source,
            owner.exposure,
            resources.color_lut,
            linear_clamp_sampler
        )
        .Draw(
            "ToneMapping",
            Rect2D(
                0,
                0,
                resources.target->GetExtent().x,
                resources.target->GetExtent().y
            ),
            std::move(full_screen_draw_data),
            ColorAttachment(resources.target)
        );
}

void ToneMappingPass::Process(
    CommandList& cmd_list,
    Params       params,
    TextureRef   source_texture,
    TextureRef   target_texture
) {
    const PreparedCommand command =
        Prepare(params, source_texture);
    const RecordResources resources =
        CaptureResources(
            std::move(source_texture),
            std::move(target_texture)
        );
    const auto owner = recording_owner;

    cmd_list.PushScopeWithTimeScope("ToneMappingPass");
    RecordConstantsUpload(cmd_list, *owner, command);
    if (command.reset_exposure) {
        RecordResetExposure(cmd_list, *owner);
    }
    RecordResetHistogram(cmd_list, *owner);
    RecordHistogram(cmd_list, *owner, command, resources);
    RecordExposure(cmd_list, *owner);
    RecordRender(cmd_list, *owner, resources);
    cmd_list.PopScopeWithTimeScope();
}

bool ToneMappingPass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx,
    Params                       params,
    TextureRef                   source_texture,
    TextureRef                   target_texture
) {
    const auto owner = recording_owner;
    if (!owner || !owner->constants || !owner->histogram ||
        !owner->exposure || !source_texture || !target_texture ||
        source_texture->GetExtent() != target_texture->GetExtent() ||
        source_texture.Get() != rt_ctx.frame_rt.resolved_color.Get() ||
        target_texture.Get() != rt_ctx.frame_rt.ldr_color.Get() ||
        !graph_resources.resolved_color.IsValid() ||
        !graph_resources.ldr_color.IsValid()) {
        return false;
    }

    const PreparedCommand command =
        Prepare(params, source_texture);
    const RecordResources resources =
        CaptureResources(
            std::move(source_texture),
            std::move(target_texture)
        );
    if (!resources.source || !resources.target || !resources.color_lut) {
        return false;
    }

    const auto constants = ImportRTGraphBuffer(
        graph,
        "RT.ToneMapping.constants",
        owner->constants
    );
    const auto histogram = ImportRTGraphBuffer(
        graph,
        "RT.ToneMapping.histogram",
        owner->histogram
    );
    const auto exposure = ImportRTGraphBuffer(
        graph,
        "RT.ToneMapping.exposure",
        owner->exposure
    );

    RenderGraph::TextureHandle lut = graph_resources.resolved_color;
    const bool lut_is_source =
        resources.color_lut.Get() == resources.source.Get();
    if (!lut_is_source) {
        lut = ImportRTGraphTexture(
            graph,
            "RT.ToneMapping.color_lut",
            resources.color_lut
        );
    }
    if (!lut.IsValid()) {
        return false;
    }

    const auto initial_buffer =
        [&](RenderGraph::BufferHandle buffer) {
            graph.SetInitialState(
                buffer,
                initialized ?
                    RenderGraph::BufferState::ShaderResource :
                    RenderGraph::BufferState::Undefined,
                initialized ?
                    RenderGraph::QueueRole::Graphics :
                    RenderGraph::QueueRole::None,
                initialized ?
                    RenderGraph::AccessMode::Read :
                    RenderGraph::AccessMode::None
            );
        };
    initial_buffer(constants);
    initial_buffer(histogram);
    initial_buffer(exposure);

    graph.SetInitialState(
        graph_resources.resolved_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        graph_resources.ldr_color,
        initialized ?
            RenderGraph::TextureState::Sampled :
            RenderGraph::TextureState::Undefined,
        initialized ?
            RenderGraph::QueueRole::Graphics :
            RenderGraph::QueueRole::None,
        initialized ?
            RenderGraph::AccessMode::Read :
            RenderGraph::AccessMode::None
    );
    if (!lut_is_source) {
        graph.SetInitialState(
            lut,
            PreferredReadState(resources.color_lut),
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    graph.AddRecordPass(
        "RT.ToneMapping.UploadConstants",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Copy
                )
                .Write(
                    constants,
                    RenderGraph::BufferState::TransferDestination
                );
        },
        [owner, command](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT ToneMapping Constants Upload",
                GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(cmd_list, *owner, command);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    if (command.reset_exposure) {
        graph.AddRecordPass(
            "RT.ToneMapping.ResetExposure",
            [=](RenderGraph::PassBuilder& builder) {
                builder
                    .ExecuteOn(
                        RenderGraph::QueueRole::Graphics,
                        RenderGraph::PipelineType::Copy
                    )
                    .Write(
                        exposure,
                        RenderGraph::BufferState::TransferDestination
                    );
            },
            [owner](CommandList& cmd_list) {
                ScopedGpuMarker marker(
                    cmd_list,
                    "Pass: RT ToneMapping Reset Exposure",
                    GpuMarkerPalette::Transfer()
                );
                RecordResetExposure(cmd_list, *owner);
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }

    graph.AddRecordPass(
        "RT.ToneMapping.ResetHistogram",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Copy
                )
                .Write(
                    histogram,
                    RenderGraph::BufferState::TransferDestination
                );
        },
        [owner](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT ToneMapping Reset Histogram",
                GpuMarkerPalette::Transfer()
            );
            RecordResetHistogram(cmd_list, *owner);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.ToneMapping.Histogram",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Compute
                )
                .Read(
                    constants,
                    RenderGraph::BufferState::ShaderResource
                )
                .Read(
                    graph_resources.resolved_color,
                    RenderGraph::TextureState::Sampled
                )
                .ReadWrite(
                    histogram,
                    RenderGraph::BufferState::UnorderedAccess
                );
        },
        [owner, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT ToneMapping Histogram",
                GpuMarkerPalette::Pass()
            );
            RecordHistogram(cmd_list, *owner, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.ToneMapping.Exposure",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Compute
                )
                .Read(
                    constants,
                    RenderGraph::BufferState::ShaderResource
                )
                .Read(
                    histogram,
                    RenderGraph::BufferState::ShaderResource
                )
                .ReadWrite(
                    exposure,
                    RenderGraph::BufferState::UnorderedAccess
                );
        },
        [owner](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT ToneMapping Exposure",
                GpuMarkerPalette::Pass()
            );
            RecordExposure(cmd_list, *owner);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.ToneMapping.Render",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Graphics
                )
                .Read(
                    constants,
                    RenderGraph::BufferState::ShaderResource
                )
                .Read(
                    graph_resources.resolved_color,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    exposure,
                    RenderGraph::BufferState::ShaderResource
                )
                .Write(
                    graph_resources.ldr_color,
                    RenderGraph::TextureState::RenderTarget
                );
            if (!lut_is_source) {
                builder.Read(
                    lut,
                    RenderGraph::TextureState::Sampled
                );
            }
        },
        [owner, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "ToneMappingPass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            RecordRender(cmd_list, *owner, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        graph_resources.resolved_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        graph_resources.ldr_color,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        histogram,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        exposure,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    if (!lut_is_source) {
        graph.Export(
            lut,
            PreferredReadState(resources.color_lut),
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    return true;
}

void ToneMappingPass::CommitAcceptedFrame(
    float elapsed_time,
    bool  enabled
) {
    initialized          = true;
    tone_mapping_enabled = enabled;
    frame_time =
        std::max(FiniteOr(elapsed_time, 0.f), 0.f);
    ++frame_idx;
}

} // namespace Moer::Render::Raytracing
