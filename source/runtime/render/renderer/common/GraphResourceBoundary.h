#pragma once

#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include <unordered_set>

namespace Moer::Render {

// Persistent graphics resources have one explicit boundary between graphs.
// Their owners establish it at allocation; each graph exports it again. There
// is no backend preferred-state restoration or independently maintained ledger.
inline RenderGraph::TextureState GraphicsTextureBoundary(const Texture& texture) {
    return uint32(texture.GetUsage() & ETextureUsageFlags::PRESENTATION_SOURCE) ?
               RenderGraph::TextureState::PresentationSource :
               RenderGraph::TextureState::Sampled;
}

inline void InitializeGraphicsTextures(std::span<const TextureRef> textures) {
    if (textures.empty()) {
        return;
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
        const bool presentation =
            GraphicsTextureBoundary(*texture) == RenderGraph::TextureState::PresentationSource;
        barriers.push_back(BarrierCreateInfo::Transition(
            view,
            BarrierState{},
            BarrierState::Texture(
                presentation ? ERHIPipelineStageFlags::PS_TRANSFER : ERHIPipelineStageFlags::PS_ALL_COMMANDS,
                presentation ? ERHIAccessFlags::TRANSFER_READ : ERHIAccessFlags::SHADER_READ,
                presentation ? ETextureLayout::TEXTURE_LAYOUT_COMMON :
                               ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            ),
            texture->GetAspectFlags()
        ));
    }
    if (barriers.empty()) {
        return;
    }
    commands.Barriers(barriers);
    commands.AddCallback([owners = std::vector<TextureRef>(textures.begin(), textures.end())] {});
    RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
}

inline void InitializeGraphicsTexture(TextureRef texture) {
    InitializeGraphicsTextures(std::span<const TextureRef>(&texture, 1));
}

} // namespace Moer::Render
