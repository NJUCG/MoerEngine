#pragma once

#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include "rhi/executor/RHIExecutor.h"
#include <stdexcept>
#include <unordered_set>

namespace Moer::Render {

// Persistent graphics resources have one explicit boundary between graphs.
// Their owners establish it at allocation; each graph exports it again. There
// is no backend preferred-state restoration. Accepted snapshots validate this
// declared boundary; they do not choose a resource's next state.
inline void InitializeGraphicsTextures(std::span<const TextureRef> textures, RenderGraph::TextureAccessType boundary = RenderGraph::TextureAccessType::Sampled) {
    if (textures.empty()) {
        return;
    }
    if (boundary != RenderGraph::TextureAccessType::Sampled && boundary != RenderGraph::TextureAccessType::TransferSource) {
        throw std::invalid_argument("graphics texture initialization requires a sampled or transfer-source boundary");
    }
    CommandList commands;
    commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
    std::unordered_set<Texture*>   initialized;
    std::vector<BarrierCreateInfo> barriers;
    for (const auto& texture : textures) {
        if (!texture || !initialized.emplace(texture.Get()).second) {
            continue;
        }
        TextureView view(texture);
        view.num_mips  = texture->GetNumMips();
        view.num_array = texture->GetNumArray();
        const bool transfer_source = boundary == RenderGraph::TextureAccessType::TransferSource;
        barriers.push_back(BarrierCreateInfo::Transition(
            view,
            BarrierState{},
            BarrierState::Texture(
                transfer_source ? ERHIPipelineStageFlags::PS_TRANSFER : ERHIPipelineStageFlags::PS_ALL_COMMANDS,
                transfer_source ? ERHIAccessFlags::TRANSFER_READ : ERHIAccessFlags::SHADER_READ,
                transfer_source ? ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC :
                                  ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            ),
            texture->GetAspectFlags()
        ));
    }
    if (barriers.empty()) {
        return;
    }
    commands.Barriers(barriers);
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
}

inline void InitializeGraphicsTexture(TextureRef texture, RenderGraph::TextureAccessType boundary = RenderGraph::TextureAccessType::Sampled) {
    InitializeGraphicsTextures(std::span<const TextureRef>(&texture, 1), boundary);
}

} // namespace Moer::Render
