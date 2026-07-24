#include "VisualizePass.h"

// 将选定的调试视图写入渲染器的调试输出纹理。

#include "RTResource.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

#include <cstring>

namespace Moer::Render::Raytracing {

VisualizePass::VisualizePass(RenderDevice& device, ShaderManager& manager) {
    auto owner = MakeShared<RecordingOwner>();
    owner->pipeline =
        manager.Compute<VisualizePipeline>("pipelines/raytracing/passes/VisualizePass.hlsl");
    owner->constants = device.CreateBuffer<Moer::byte>(
        "Raytracing::VisualizeBuffer", sizeof(VisualizeParams), EBufferUsageFlags::CONSTANT_BUFFER
    );
    recording_owner = std::move(owner);
}

VisualizePass::PreparedCommand VisualizePass::Prepare(
    const RTContext&       rt_ctx,
    const VisualizeConfig& config
) const {
    PreparedCommand command{};
    command.params.grid_params    = rt_ctx.is_ctx.GetGridParams();
    command.params.b_split        = config.b_split;
    command.params.split_ratio    = config.split_ratio;
    command.params.visualize_mode = config.visualize_mode;
    command.params.main_view      = rt_ctx.main_view;
    command.params.output_size =
        rt_ctx.frame_rt.debug_color->GetExtent().xy;
    command.dispatch_groups = uint3(
        (command.params.output_size.x + 15) / 16,
        (command.params.output_size.y + 15) / 16,
        1
    );
    return command;
}

VisualizePass::RecordResources VisualizePass::CaptureResources(
    const RTContext& rt_ctx
) {
    const FrameResources& frame = rt_ctx.frame_rt;
    return RecordResources{
        .ldr_color         = frame.ldr_color,
        .diffuse_lighting  = frame.diffuse_lighting,
        .specular_lighting = frame.specular_lighting,
        .view_depth =
            rt_ctx.b_current_frame ? frame.view_depth : frame.prev_view_depth,
        .clip_depth = frame.clip_depth,
        .emission   = frame.emission,
        .normal =
            rt_ctx.b_current_frame ? frame.normal : frame.prev_normal,
        .specular_roughness =
            rt_ctx.b_current_frame ?
                frame.specular_roughness :
                frame.prev_specular_roughness,
        .motion           = frame.motion,
        .normal_roughness = frame.normal_roughness,
        .debug_color      = frame.debug_color
    };
}

void VisualizePass::RecordConstantsUpload(
    CommandList&           cmd_list,
    const RecordingOwner&  owner,
    const PreparedCommand& command
) {
    Array<Moer::byte> upload_data(sizeof(VisualizeParams));
    std::memcpy(
        upload_data.data(),
        &command.params,
        sizeof(VisualizeParams)
    );
    cmd_list.CopyFrom(std::move(upload_data), owner.constants->GetView());
}

void VisualizePass::RecordVisualize(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Compute(
            owner.pipeline,
            owner.constants,
            resources.ldr_color,
            resources.diffuse_lighting,
            resources.specular_lighting,
            resources.view_depth,
            resources.clip_depth,
            resources.emission,
            resources.normal,
            resources.specular_roughness,
            resources.motion,
            resources.normal_roughness,
            resources.debug_color
        )
        .Dispatch(command.dispatch_groups, "Visualize");
}

void VisualizePass::Process(
    CommandList&           cmd_list,
    RTContext&             rt_ctx,
    const VisualizeConfig& config
) {
    const PreparedCommand command   = Prepare(rt_ctx, config);
    const RecordResources resources = CaptureResources(rt_ctx);
    const auto            owner     = recording_owner;

    cmd_list.PushScopeWithTimeScope("VisualizePass");
    RecordConstantsUpload(cmd_list, *owner, command);
    RecordVisualize(cmd_list, *owner, command, resources);
    cmd_list.PopScopeWithTimeScope();
}

bool VisualizePass::AddPasses(
    RenderGraph&                 graph,
    const RTGraphFrameResources& graph_resources,
    const RTContext&             rt_ctx,
    const VisualizeConfig&       config
) {
    const auto owner = recording_owner;
    if (!owner || !owner->constants || !graph_resources.ldr_color.IsValid() ||
        !graph_resources.diffuse_lighting.IsValid() ||
        !graph_resources.specular_lighting.IsValid() ||
        !graph_resources.current_view_depth.IsValid() ||
        !graph_resources.clip_depth.IsValid() ||
        !graph_resources.emission.IsValid() ||
        !graph_resources.current_normal.IsValid() ||
        !graph_resources.current_specular_roughness.IsValid() ||
        !graph_resources.motion.IsValid() ||
        !graph_resources.normal_roughness.IsValid() ||
        !graph_resources.debug_color.IsValid()) {
        return false;
    }

    const PreparedCommand command   = Prepare(rt_ctx, config);
    const RecordResources resources = CaptureResources(rt_ctx);
    if (!resources.debug_color) {
        return false;
    }
    const auto expected_extent = resources.debug_color->GetExtent();
    for (const auto& texture : {
             resources.ldr_color,
             resources.diffuse_lighting,
             resources.specular_lighting,
             resources.view_depth,
             resources.clip_depth,
             resources.emission,
             resources.normal,
             resources.specular_roughness,
             resources.motion,
             resources.normal_roughness,
             resources.debug_color,
         }) {
        if (!texture || texture->GetExtent() != expected_extent) {
            return false;
        }
    }
    if (expected_extent.x == 0 || expected_extent.y == 0 ||
        resources.ldr_color.Get() != rt_ctx.frame_rt.ldr_color.Get() ||
        resources.debug_color.Get() != rt_ctx.frame_rt.debug_color.Get()) {
        return false;
    }

    const auto constants = ImportRTGraphBuffer(
        graph,
        "RT.Visualize.constants",
        owner->constants
    );
    graph.SetInitialState(
        constants,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.SetInitialState(
        graph_resources.debug_color,
        RenderGraph::TextureState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );

    graph.AddRecordPass(
        "RT.Visualize.UploadConstants",
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
                "Pass: RT Visualize Constants Upload",
                GpuMarkerPalette::Transfer()
            );
            RecordConstantsUpload(cmd_list, *owner, command);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.Visualize.Dispatch",
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
                    graph_resources.ldr_color,
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
                .Read(
                    graph_resources.current_view_depth,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.clip_depth,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.emission,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_normal,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.current_specular_roughness,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.motion,
                    RenderGraph::TextureState::Sampled
                )
                .Read(
                    graph_resources.normal_roughness,
                    RenderGraph::TextureState::Sampled
                )
                .Write(
                    graph_resources.debug_color,
                    RenderGraph::TextureState::UnorderedAccess
                );
        },
        [owner, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "VisualizePass",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            RecordVisualize(cmd_list, *owner, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        graph_resources.debug_color,
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
