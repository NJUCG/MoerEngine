#include "CompositionPass.h"

// 选择当前使用的 GBuffer 历史，并合成 HDR 光照结果。

#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/ShaderParameters.h"

#include <cmath>
#include <cstring>

namespace Moer::Render::Raytracing {

CompositionPass::CompositionPass(
    RenderDevice&    device,
    ShaderManager&   manager,
    BindlessArrayRef bindless_array
) : bindless_array(std::move(bindless_array)) {

    constexpr int with_nrd = WITH_NRD;
#pragma push_macro("WITH_NRD")
#undef WITH_NRD
    CompositionPassPipeline::MutationSet mutation_set;
    mutation_set.SetMutation<CompositionPassPipeline::WITH_NRD>(with_nrd);
#pragma pop_macro("WITH_NRD")

    auto owner = MakeShared<RecordingOwner>();
    owner->pipeline = manager.Compute<CompositionPassPipeline>(
        "pipelines/raytracing/passes/CompositionPass.hlsl", mutation_set
    );

    owner->constants = device.CreateBuffer<Moer::byte>(
        "CompositionPass::constant_buffer", sizeof(CompositingConstants), EBufferUsageFlags::CONSTANT_BUFFER
    );
    recording_owner = std::move(owner);
}

CompositionPass::PreparedCommand
CompositionPass::Prepare(const RTContext& rt_ctx) const {
    PreparedCommand command{};
    command.constants.denoiser_mode  = rt_ctx.config.denoiser_mode;
    command.constants.enable_env_map = rt_ctx.scene_params.enable_env_map ? 1 : 0;
    command.constants.env_map_handle = rt_ctx.scene_params.env_map_handle;
    command.constants.env_rotation   = rt_ctx.scene_params.env_map_rotation;
    command.constants.env_scale      = rt_ctx.scene_params.env_map_scale;
    command.constants.main_view      = rt_ctx.main_view;
    command.constants.prev_view      = rt_ctx.prev_view;
    command.dispatch_groups = uint3(
        static_cast<uint>(std::ceil(command.constants.main_view.rect.x / 8.0f)),
        static_cast<uint>(std::ceil(command.constants.main_view.rect.y / 8.0f)),
        1
    );
    return command;
}

CompositionPass::RecordResources
CompositionPass::CaptureResources(const RTContext& rt_ctx) const {
    const bool            current_frame = rt_ctx.b_current_frame;
    const FrameResources& frame          = rt_ctx.frame_rt;
    TextureRef denoised_diffuse_lighting  = frame.diffuse_lighting;
    TextureRef denoised_specular_lighting = frame.specular_lighting;
#if WITH_NRD
    if (IsNrdDenoiserActive(rt_ctx.config.denoiser_mode)) {
        denoised_diffuse_lighting  = frame.denoised_diffuse_lighting;
        denoised_specular_lighting = frame.denoised_specular_lighting;
    }
#endif
    return RecordResources{
        .hdr_color          = frame.hdr_color,
        .motion             = frame.motion,
        .view_depth         = current_frame ? frame.view_depth : frame.prev_view_depth,
        .diffuse_albedo     =
            current_frame ? frame.diffuse_albedo : frame.prev_diffuse_albedo,
        .specular_roughness =
            current_frame ? frame.specular_roughness : frame.prev_specular_roughness,
        .normal = current_frame ? frame.normal : frame.prev_normal,
        .emission                  = frame.emission,
        .diffuse_lighting          = frame.diffuse_lighting,
        .specular_lighting         = frame.specular_lighting,
        .denoised_diffuse_lighting  = std::move(denoised_diffuse_lighting),
        .denoised_specular_lighting = std::move(denoised_specular_lighting),
        .env_map        = rt_ctx.env_map,
        .bindless_array = bindless_array
    };
}

void CompositionPass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const RecordingOwner&  owner,
    const PreparedCommand& command
) {
    Array<byte> upload_data(sizeof(CompositingConstants));
    std::memcpy(
        upload_data.data(),
        &command.constants,
        sizeof(CompositingConstants)
    );
    cmd_list.CopyFrom(std::move(upload_data), owner.constants->GetView());
}

void CompositionPass::RecordComposition(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Compute(
            owner.pipeline,
            owner.constants,
            resources.hdr_color,
            resources.motion,
            resources.view_depth,
            resources.diffuse_albedo,
            resources.specular_roughness,
            resources.normal,
            resources.emission,
            resources.diffuse_lighting,
            resources.specular_lighting,
            resources.denoised_diffuse_lighting,
            resources.denoised_specular_lighting,
            resources.bindless_array
        )
        .Dispatch(command.dispatch_groups, "CompositionPass");
}

void CompositionPass::Process(CommandList& cmd_list, RTContext& rt_ctx) {
    const PreparedCommand command   = Prepare(rt_ctx);
    const RecordResources resources = CaptureResources(rt_ctx);
    const auto            owner     = recording_owner;
    RecordConstantsUpload(cmd_list, *owner, command);
    RecordComposition(cmd_list, *owner, command, resources);
}

bool CompositionPass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx
) {
    const PreparedCommand command   = Prepare(rt_ctx);
    const RecordResources resources = CaptureResources(rt_ctx);
    const auto            owner     = recording_owner;
    const bool nrd_active =
#if WITH_NRD
        IsNrdDenoiserActive(rt_ctx.config.denoiser_mode);
#else
        false;
#endif
    if (!owner || !owner->constants || !resources.hdr_color ||
        !resources.motion || !resources.view_depth ||
        !resources.diffuse_albedo || !resources.specular_roughness ||
        !resources.normal || !resources.emission ||
        !resources.diffuse_lighting || !resources.specular_lighting ||
        !resources.denoised_diffuse_lighting ||
        !resources.denoised_specular_lighting || !resources.bindless_array ||
        !graph_resources.frame_setup.ready.IsValid() ||
        (nrd_active &&
         (!graph_resources.denoised_diffuse_lighting.IsValid() ||
          !graph_resources.denoised_specular_lighting.IsValid() ||
          !graph_resources.nrd_ready.IsValid())) ||
        (command.constants.enable_env_map != 0 &&
         (!resources.env_map || !graph_resources.env_map.IsValid()))) {
        return false;
    }

    const auto constants = ImportRTGraphBuffer(
        graph,
        "RT.Composition.constants",
        owner->constants
    );
    graph.SetInitialState(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        graph_resources.hdr_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    graph.AddRecordPass(
        "RT.Composition.UploadConstants",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(
                       RenderGraph::QueueRole::Graphics,
                       RenderGraph::PipelineType::Copy
                   )
                .Write(constants, RenderGraph::BufferState::TransferDestination);
        },
        [owner, command](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "Pass: RT Composition Constants Upload",
                GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(cmd_list, *owner, command);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.Composition.Dispatch",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Compute
                )
                .Read(constants, RenderGraph::BufferState::ShaderResource)
                .Read(graph_resources.frame_setup.ready)
                .Write(
                    graph_resources.hdr_color,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .ReadWrite(
                    graph_resources.motion,
                    RenderGraph::TextureState::UnorderedAccess
                )
                .Read(
                    graph_resources.current_view_depth,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_diffuse_albedo,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_specular_roughness,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_normal,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.emission,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.diffuse_lighting,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.specular_lighting,
                    RenderGraph::TextureState::Sampled
                );
            if (nrd_active) {
                builder
                    .Read(
                        graph_resources.denoised_diffuse_lighting,
                        RenderGraph::TextureState::Sampled
                    )
                    .Read(
                        graph_resources.denoised_specular_lighting,
                        RenderGraph::TextureState::Sampled
                    )
                    .Read(graph_resources.nrd_ready);
            }
            if (graph_resources.env_map.IsValid()) {
                builder.Read(
                    graph_resources.env_map,
                    RenderGraph::TextureState::Sampled
                );
            }
        },
        [owner, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "CompositionPass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            RecordComposition(cmd_list, *owner, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        graph_resources.hdr_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

} // namespace Moer::Render::Raytracing
