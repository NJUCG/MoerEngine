#include "ShowTexturePass.h"

#include "PixelFormat.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"

#include <algorithm>

namespace Moer::Render::Raytracing {

namespace {

bool CanRecordShowTexture(
    const TextureRef& source,
    const TextureRef& target
) {
    return source && target && source.Get() != target.Get() &&
           source->GetDimension() == ETextureDimension::TEX_2D &&
           source->GetNumArray() == 1 && source->GetNumMips() > 0 &&
           source->GetExtent().x > 0 && source->GetExtent().y > 0 &&
           target->GetExtent().x > 0 && target->GetExtent().y > 0;
}

} // namespace

ShowTexturePass::ShowTexturePass(
    ShaderManager&    manager,
    BindlessArrayRef  bindless_array
) {
    auto owner = MakeShared<RecordingOwner>();
    owner->bindless_array = std::move(bindless_array);

    GfxPsoCreateInfo pso_info(
        RHIRasterizeInfo::Preset(),
        {},
        {RHIColorAttachmentInfo::Preset(PF_R8G8B8A8_UNORM)}
    );
    owner->pipeline =
        manager.Raster()
            .Vertex("core/utils/FullScreenQuad.hlsl")
            .Pixel("core/utils/ShowTexture.frag.hlsl")
            .Build<ShowTexturePipeline>(std::move(pso_info));
    recording_owner = std::move(owner);
}

ShowTexturePass::PreparedCommand ShowTexturePass::Prepare(
    ShowTextureParams params,
    const TextureRef& source,
    const TextureRef& target
) {
    PreparedCommand command{};
    command.params         = params;
    command.params.dst_dim = target->GetExtent().xy;
    command.params.mip_level =
        std::min(command.params.mip_level, source->GetNumMips() - 1);
    return command;
}

ShowTexturePass::RecordResources ShowTexturePass::CaptureResources(
    const TextureRef& source,
    const TextureRef& target
) {
    return {
        .source = source,
        .target = target,
    };
}

void ShowTexturePass::Record(
    CommandList&           cmd_list,
    RecordingOwner&        owner,
    const PreparedCommand& command,
    const RecordResources& resources
) {
    cmd_list
        .Gfx(
            owner.pipeline,
            command.params,
            resources.source->GetView(0, resources.source->GetNumMips()),
            owner.bindless_array
        )
        .Draw(
            "ShowTexture",
            Rect2D(
                0,
                0,
                resources.target->GetExtent().x,
                resources.target->GetExtent().y
            ),
            {},
            3,
            {SingleDrawParam(3, 1, 0, 0, 0)},
            ColorAttachment(resources.target)
        );
}

void ShowTexturePass::Process(
    CommandList&      cmd_list,
    ShowTextureParams params,
    const TextureRef& source,
    const TextureRef& target
) {
    const auto owner = recording_owner;
    if (!owner || !owner->bindless_array ||
        !CanRecordShowTexture(source, target)) {
        return;
    }
    const PreparedCommand command   = Prepare(params, source, target);
    const RecordResources resources = CaptureResources(source, target);
    Record(cmd_list, *owner, command, resources);
}

bool ShowTexturePass::AddPass(
    RenderGraph&               graph,
    RenderGraph::TextureHandle source_handle,
    RenderGraph::TextureHandle target_handle,
    ShowTextureParams          params,
    const TextureRef&          source,
    const TextureRef&          target
) {
    const auto owner = recording_owner;
    if (!owner || !owner->bindless_array ||
        !CanRecordShowTexture(source, target) ||
        !source_handle.IsValid() || !target_handle.IsValid()) {
        return false;
    }

    const PreparedCommand command   = Prepare(params, source, target);
    const RecordResources resources = CaptureResources(source, target);
    constexpr RenderGraph::TextureState source_state =
        RenderGraph::TextureState::Sampled;

    graph.SetInitialState(
        source_handle,
        source_state,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddRecordPass(
        "RT.ShowTexture",
        [=](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    RenderGraph::PipelineType::Graphics
                )
                .Read(source_handle, RenderGraph::TextureState::Sampled)
                .Write(target_handle, RenderGraph::TextureState::RenderTarget);
        },
        [owner, command, resources](CommandList& cmd_list) {
            ScopedGpuMarker marker(
                cmd_list,
                "ShowTexture",
                GpuMarkerPalette::Pass(),
                EGpuMarkerMode::Timestamp
            );
            Record(cmd_list, *owner, command, resources);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.Export(
        source_handle,
        source_state,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        target_handle,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

} // namespace Moer::Render::Raytracing
