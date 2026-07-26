#include "NrdDenoisePass.h"

#if WITH_NRD

#include "rhi/RHICommand.h"

namespace Moer::Render::Raytracing {

NrdDenoisePass::PreparedCommand NrdDenoisePass::Prepare(
    SharedPtr<Ext::NRDInterface> interface,
    uint32                       frame_index,
    const Vector2ui&             size,
    const Vector2f&              jitter,
    const Matrix4x4f&            view,
    const Matrix4x4f&            projection,
    nrd::Denoiser                denoiser,
    const RTContext&             rt_ctx
) {
    if (!interface) {
        return {};
    }

    const auto& frame = rt_ctx.frame_rt;
    Ext::NRDInterface::FrameDesc desc{
        .frame_index = frame_index,
        .size         = size,
        .jitter       = jitter,
        .view         = view,
        .projection   = projection,
        .denoiser     = denoiser,
    };
    desc
        .Set(
            Ext::NRDInterface::EResourceSlot::MOTION_VECTOR,
            frame.motion
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS,
            frame.normal_roughness
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::VIEW_Z,
            rt_ctx.b_current_frame ?
                frame.view_depth :
                frame.prev_view_depth
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::IN_DIFFUSE,
            frame.diffuse_lighting
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::IN_SPECULAR,
            frame.specular_lighting
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE,
            frame.denoised_diffuse_lighting
        )
        .Set(
            Ext::NRDInterface::EResourceSlot::OUT_SPECULAR,
            frame.denoised_specular_lighting
        );

    auto prepared_frame = interface->PrepareFrame(std::move(desc));
    return PreparedCommand{
        .interface        = std::move(interface),
        .frame            = std::move(prepared_frame),
        .submission_fence = RenderDevice::Get().CreateFence(),
        .submission_value = 1,
        .graph_submission_fence =
            RenderDevice::Get().CreateFence(),
    };
}

bool NrdDenoisePass::AddPasses(
    RenderGraph&           graph,
    RTGraphFrameResources& graph_resources,
    const PreparedCommand& command,
    bool                   outputs_initialized
) {
    if (!outputs_initialized || !command.IsValid() ||
        !graph_resources.frame_setup.ready.IsValid() ||
        !graph_resources.motion.IsValid() ||
        !graph_resources.normal_roughness.IsValid() ||
        !graph_resources.current_view_depth.IsValid() ||
        !graph_resources.diffuse_lighting.IsValid() ||
        !graph_resources.specular_lighting.IsValid() ||
        !graph_resources.denoised_diffuse_lighting.IsValid() ||
        !graph_resources.denoised_specular_lighting.IsValid()) {
        return false;
    }

    const auto same_texture =
        [&](RenderGraph::TextureHandle graph_texture,
            Ext::NRDInterface::EResourceSlot slot) {
            const TextureRef physical =
                graph.GetPhysicalTexture(graph_texture);
            return physical &&
                   physical.Get() ==
                       command.frame->GetResource(slot).Get();
        };
    if (!same_texture(
            graph_resources.motion,
            Ext::NRDInterface::EResourceSlot::MOTION_VECTOR
        ) ||
        !same_texture(
            graph_resources.normal_roughness,
            Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS
        ) ||
        !same_texture(
            graph_resources.current_view_depth,
            Ext::NRDInterface::EResourceSlot::VIEW_Z
        ) ||
        !same_texture(
            graph_resources.diffuse_lighting,
            Ext::NRDInterface::EResourceSlot::IN_DIFFUSE
        ) ||
        !same_texture(
            graph_resources.specular_lighting,
            Ext::NRDInterface::EResourceSlot::IN_SPECULAR
        ) ||
        !same_texture(
            graph_resources.denoised_diffuse_lighting,
            Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE
        ) ||
        !same_texture(
            graph_resources.denoised_specular_lighting,
            Ext::NRDInterface::EResourceSlot::OUT_SPECULAR
        )) {
        return false;
    }

    graph.SetInitialState(
        graph_resources.denoised_diffuse_lighting,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        graph_resources.denoised_specular_lighting,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    const auto ready = graph.CreateTransientToken("RT.NRD.ready");
    const auto pass = graph.AddRecordPass(
        "RT.NRD",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Compute
                )
                .Read(graph_resources.frame_setup.ready)
                .Read(
                    graph_resources.motion,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.normal_roughness,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_view_depth,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.diffuse_lighting,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.specular_lighting,
                    RenderGraph::TextureState::Sampled
                )
                .ReadWrite(
                    graph_resources.denoised_diffuse_lighting,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .ReadWrite(
                    graph_resources.denoised_specular_lighting,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Write(ready)
                .SideEffect()
                .SerialRecord()
                .TranslateSerialControl();
        },
        [command](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: Radiance Denoising",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            Process(cmd_list, command);
        },
        RenderGraph::PassExecutionClass::SerialRecord
    );
    if (!pass.IsValid()) {
        return false;
    }

    graph_resources.nrd_ready = ready;
    graph_resources.nrd_pass  = pass;
    graph.Export(
        graph_resources.denoised_diffuse_lighting,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        graph_resources.denoised_specular_lighting,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

void NrdDenoisePass::Process(
    CommandList&           cmd_list,
    const PreparedCommand& command
) {
    if (!command.IsValid()) {
        throw std::invalid_argument(
            "NRD pass requires a valid immutable prepared command"
        );
    }
    command.interface->Denoise(
        cmd_list,
        command.frame,
        "Radiance Denoising"
    );
    // This signal belongs to the same immutable CmdSubmit as the NRD custom
    // command. Fence::WaitSubmitted therefore distinguishes native queue
    // acceptance from frontend recording-handoff admission.
    cmd_list.Signal(
        command.submission_fence,
        command.submission_value
    );
}

bool NrdDenoisePass::CommitAcceptedSubmission(
    const PreparedCommand& command,
    const FenceRef&        frame_fence,
    uint64                 frame_value,
    uint64                 graph_submission_value_count
) {
    if (!command.IsValid() || !frame_fence || frame_value == 0 ||
        !frame_fence->WaitSubmitted(frame_value)) {
        return false;
    }
    // A recoverably rejected graph source must not be hidden by a later
    // accepted final tail. Every managed source receives a monotonically
    // increasing value on this transaction fence; inspect each value because
    // a later accepted value does not erase an earlier explicit rejection.
    for (uint64 value = 1; value <= graph_submission_value_count;
         ++value) {
        if (!command.graph_submission_fence->WaitSubmitted(value)) {
            return false;
        }
    }
    if (!command.submission_fence->WaitSubmitted(
            command.submission_value
        )) {
        return false;
    }
    return command.interface->CommitFrame(command.frame);
}

} // namespace Moer::Render::Raytracing

#endif
