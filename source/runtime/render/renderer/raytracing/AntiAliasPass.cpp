#include "AntiAliasPass.h"
// 实现时序解析与确定性抖动序列。

#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/postprocess/ShaderParameters.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace Moer::Render::Raytracing {

AntialiasPass::AntialiasPass(RenderDevice& _device, ShaderManager& _manager, CreateInfo _info) :
    frame_idx(0),
    jitter(float2(0.f)),
    jitter_mode(EJitter::MSAA),
    motion(_info.motion),
    feedback_color_ping(_info.feedback_color_ping),
    feedback_color_pong(_info.feedback_color_pong),
    feedback_slot_zero(_info.feedback_color_ping),
    feedback_slot_one(_info.feedback_color_pong) {
    auto owner = MakeShared<RecordingOwner>();
    owner->pipeline =
        _manager.Compute<TAAPipeline>("pipelines/raytracing/postprocess/TAAPass.hlsl");
    owner->constants = _device.CreateBuffer<Moer::byte>(
        "PostProcess::TAAConstantBuffer", sizeof(TAAParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
    recording_owner = std::move(owner);
}

AntialiasPass::PreparedCommand AntialiasPass::Prepare(
    Params            params,
    bool              prev_view_valid,
    const TextureRef& input,
    const TextureRef& output
) const {
    PreparedCommand command{};
    command.params.in_view_origin = float2(0.f);
    command.params.in_view_size =
        float2(input->GetExtent().x, input->GetExtent().y);
    command.params.out_view_origin = float2(0.f);
    command.params.out_view_size =
        float2(output->GetExtent().x, output->GetExtent().y);
    command.params.in_pixel_offset      = GetPixelOffset();
    command.params.out_texture_size_inv = float2(
        1.f / output->GetExtent().x,
        1.f / output->GetExtent().y
    );
    command.params.input_over_output_size =
        command.params.in_view_size / command.params.out_view_size;
    command.params.output_over_input_size =
        command.params.out_view_size / command.params.in_view_size;
    command.params.clamping_factor =
        params.enable_history_clamp ? params.clamping_factor : -1.f;
    command.params.new_frame_weight =
        prev_view_valid ? params.new_frame_weight : 1.f;
    command.params.pqc =
        std::clamp(params.max_radiance, 1e-4f, 1e8f);
    command.params.inv_pqc = 1.f / command.params.pqc;
    command.dispatch_groups = uint3(
        (output->GetExtent().x + 15) / 16,
        (output->GetExtent().y + 15) / 16,
        1
    );
    return command;
}

AntialiasPass::RecordResources AntialiasPass::CaptureResources(
    TextureRef input,
    TextureRef output
) const {
    return RecordResources{
        .input          = std::move(input),
        .output         = std::move(output),
        .motion         = motion,
        .feedback_read  = feedback_color_ping,
        .feedback_write = feedback_color_pong
    };
}

void AntialiasPass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const RecordingOwner&  owner,
    const PreparedCommand& command
) {
    Array<Moer::byte> upload_data(sizeof(TAAParams));
    std::memcpy(
        upload_data.data(),
        &command.params,
        sizeof(TAAParams)
    );
    cmd_list.CopyFrom(std::move(upload_data), owner.constants->GetView());
}

void AntialiasPass::RecordTAA(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    Sampler linear_sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};
    cmd_list
        .Compute(
            owner.pipeline,
            owner.constants,
            resources.input,
            resources.motion,
            resources.feedback_read,
            linear_sampler,
            resources.output,
            resources.feedback_write
        )
        .Dispatch(command.dispatch_groups, "TAAPass");
}

void AntialiasPass::Process(
    CommandList& cmd_list,
    Params       params,
    bool         prev_view_valid,
    TextureRef   input,
    TextureRef   output
) {
    const PreparedCommand command =
        Prepare(params, prev_view_valid, input, output);
    const RecordResources resources =
        CaptureResources(std::move(input), std::move(output));
    const auto owner = recording_owner;

    cmd_list.PushScopeWithTimeScope("AntiAliasPass");
    RecordConstantsUpload(cmd_list, *owner, command);
    RecordTAA(cmd_list, *owner, command, resources);
    // Normalize the legacy write side through a sampled-read barrier before
    // the command-list tracker restores its preferred GENERAL layout. This
    // gives linear and graph frames the same external SRV/read boundary and
    // preserves graph -> linear transitions on boundary frames.
    cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        Array<ReadTexture>{
            {resources.feedback_write->GetView(), ETextureState::SAMPLE}
        }
    );
    cmd_list.PopScopeWithTimeScope();
}

bool AntialiasPass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx,
    Params                       params,
    bool                         prev_view_valid,
    TextureRef                   input,
    TextureRef                   output
) {
    const auto owner = recording_owner;
    if (!owner || !owner->constants || !input || !output || !motion ||
        !feedback_color_ping || !feedback_color_pong) {
        return false;
    }
    const PreparedCommand command =
        Prepare(params, prev_view_valid, input, output);
    const RecordResources resources =
        CaptureResources(std::move(input), std::move(output));
    if (!resources.input || !resources.output || !resources.motion ||
        !resources.feedback_read || !resources.feedback_write ||
        resources.feedback_read.Get() == resources.feedback_write.Get() ||
        resources.input.Get() != rt_ctx.frame_rt.hdr_color.Get() ||
        resources.output.Get() != rt_ctx.frame_rt.resolved_color.Get() ||
        resources.motion.Get() != rt_ctx.frame_rt.motion.Get()) {
        return false;
    }

    const auto feedback_handle =
        [&](const TextureRef& texture) -> RenderGraph::TextureHandle {
            if (texture.Get() == rt_ctx.frame_rt.feedback_color_ping.Get()) {
                return graph_resources.feedback_color_ping;
            }
            if (texture.Get() == rt_ctx.frame_rt.feedback_color_pong.Get()) {
                return graph_resources.feedback_color_pong;
            }
            return {};
        };
    const auto feedback_read  = feedback_handle(resources.feedback_read);
    const auto feedback_write = feedback_handle(resources.feedback_write);
    if (!feedback_read.IsValid() || !feedback_write.IsValid() ||
        feedback_read == feedback_write) {
        return false;
    }

    const auto constants =
        ImportRTGraphBuffer(graph, "RT.AntiAlias.constants", owner->constants);
    graph.SetInitialState(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        graph_resources.resolved_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    for (const auto feedback : {feedback_read, feedback_write}) {
        // UAV-capable legacy textures finish in the tracker's preferred
        // GENERAL layout. Linear TAA explicitly normalizes its write side to
        // a read dependency before that restore, and graph TAA exports both
        // histories to the same SRV/read boundary.
        graph.SetInitialState(
            feedback,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }

    UploadGraphParameters upload_parameters{};
    upload_parameters.constants = constants;
    upload_parameters.owner     = owner;
    upload_parameters.command   = command;
    graph.AddRecordPass(
        "RT.AntiAlias.UploadConstants",
        std::move(upload_parameters),
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                RenderGraph::QueueRole::Graphics,
                RenderGraph::PipelineType::Copy
            );
        },
        [](CommandList& cmd_list, const UploadGraphParameters& parameters) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT AntiAlias Constants Upload",
                GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(
                cmd_list,
                *parameters.owner,
                parameters.command
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    DispatchGraphParameters dispatch_parameters{};
    dispatch_parameters.constants      = constants;
    dispatch_parameters.hdr_color      = graph_resources.hdr_color;
    dispatch_parameters.motion         = graph_resources.motion;
    dispatch_parameters.feedback_read  = feedback_read;
    dispatch_parameters.resolved_color = graph_resources.resolved_color;
    dispatch_parameters.feedback_write = feedback_write;
    dispatch_parameters.owner          = owner;
    dispatch_parameters.command        = command;
    dispatch_parameters.resources      = resources;
    graph.AddRecordPass(
        "RT.AntiAlias.Dispatch",
        std::move(dispatch_parameters),
        [](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                RenderGraph::QueueRole::Graphics,
                RenderGraph::PipelineType::Compute
            );
        },
        [](CommandList& cmd_list, const DispatchGraphParameters& parameters) {
            ScopedGpuMarker marker(
                cmd_list,
                "AntiAliasPass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            RecordTAA(
                cmd_list,
                *parameters.owner,
                parameters.command,
                parameters.resources
            );
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        graph_resources.resolved_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    for (const auto feedback : {feedback_read, feedback_write}) {
        graph.Export(
            feedback,
            RenderGraph::TextureState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    }
    graph.Export(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

void AntialiasPass::CommitAcceptedFrame() {
    if (feedback_color_pong.Get() == feedback_slot_zero.Get()) {
        initialized_history_mask |= uint8(0b01);
    } else if (feedback_color_pong.Get() == feedback_slot_one.Get()) {
        initialized_history_mask |= uint8(0b10);
    }
    frame_idx++;

    if (jitter_mode == EJitter::R2) {
        static constexpr float g  = 1.32471795724474602596f;
        static constexpr float a1 = 1.f / g;
        static constexpr float a2 = 1.f / (g * g);
        jitter[0]                 = std::fmodf(jitter[0] + a1, 1.f);
        jitter[1]                 = std::fmodf(jitter[1] + a2, 1.f);
    }

    std::swap(feedback_color_ping, feedback_color_pong);
}

bool AntialiasPass::IsHistoryReadyForGraph() const {
    return initialized_history_mask == uint8(0b11);
}

namespace {

float VanderCorput(uint _idx, uint _base) {
    float v = 0.f;
    float f = 1.f / _base;
    uint  i = _idx;
    while (i > 0) {
        v += (i % _base) * f;
        i /= _base;
        f /= _base;
    }
    return v;
}

} // namespace

float2 AntialiasPass::GetPixelOffset() const {
    switch (jitter_mode) {

        case EJitter::MSAA: {
            static const float2 offsets[] = {
                float2(0.0625f, -0.1875f),
                float2(-0.0625f, 0.1875f),
                float2(0.3125f, 0.0625f),
                float2(-0.1875f, -0.3125f),
                float2(-0.3125f, 0.3125f),
                float2(-0.4375f, 0.0625f),
                float2(0.1875f, 0.4375f),
                float2(0.4375f, -0.4375f)
            };
            return offsets[frame_idx % 8];
        }
        case EJitter::Halton: {
            const uint idx = (frame_idx % 16) + 1;
            return float2(VanderCorput(idx, 2), VanderCorput(idx, 3)) - 0.5f;
        }
        case EJitter::R2: {
            return jitter - 0.5f;
        }
        case EJitter::WhiteNoise: {
            std::mt19937_64                       rng(frame_idx);
            std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
            return float2(dist(rng), dist(rng));
        }
        case EJitter::Num:
            break;
    }
    return float2(0.f);
}

void AntialiasPass::SetJitter(EJitter _jitter_mode) {
    jitter_mode = _jitter_mode;
}

} // namespace Moer::Render::Raytracing
