#pragma once

#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"

#include <stdexcept>

namespace Moer::Render {

// Upload owners provide the destination and its boundary contract. These
// helpers compile that declaration; they do not inspect recorded commands.
inline void RecordGraphicsUploadGraph(RenderGraph& graph, CommandList& commands) {
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    if (!graph.Compile() || !graph.RecordAndMergeFrontendCommands(commands, false)) {
        throw std::runtime_error("Resource upload graph: " + graph.GetCompileError());
    }
}

inline void RecordGraphicsBufferUpload(
    CommandList&          commands,
    std::span<const byte> data,
    BufferView            destination,
    std::string_view      name = "BufferUpload"
) {
    if (data.empty()) {
        return;
    }
    RenderGraph graph(name, RenderGraph::QueueTopology::FromRHI());
    BufferRef   owner(destination.GetBuffer());
    const auto  buffer = graph.ImportBuffer("Destination", owner, {.byte_size = owner->GetByteSize()});
    graph.SetInitialState(
        buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        buffer,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.AddRecordPass(
        "Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(buffer, RenderGraph::BufferState::TransferDestination);
        },
        [=](CommandList& list) {
            list.CopyFrom(data, destination, name);
        }
    );
    RecordGraphicsUploadGraph(graph, commands);
}

inline void RecordNewGraphicsTextureUpload(
    CommandList&          commands,
    std::span<const byte> data,
    TextureView           destination,
    std::string_view      name = "TextureUpload"
) {
    if (data.empty()) {
        return;
    }
    RenderGraph graph(name, RenderGraph::QueueTopology::FromRHI());
    TextureRef  owner(destination.GetTexture());
    const auto  texture = graph.ImportTexture(
        "Destination", owner, {.mip_count = owner->GetNumMips(), .layer_count = owner->GetNumArray()}
    );
    const RenderGraph::TextureRange range{
        .mip_first   = destination.mip_level,
        .mip_count   = 1,
        .layer_first = destination.array_layer,
        .layer_count = 1,
    };
    graph.SetInitialState(
        texture,
        RenderGraph::TextureState::Undefined,
        RenderGraph::QueueRole::None,
        RenderGraph::AccessMode::None,
        range
    );
    graph.Export(
        texture,
        RenderGraph::TextureState::Sampled,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read,
        range
    );
    graph.AddRecordPass(
        "Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(texture, RenderGraph::TextureState::TransferDestination, range);
        },
        [=](CommandList& list) {
            list.CopyFrom(data, destination, name);
        }
    );
    RecordGraphicsUploadGraph(graph, commands);
}

} // namespace Moer::Render
