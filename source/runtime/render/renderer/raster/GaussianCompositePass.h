#pragma once
#include "rhi/RHICommand.h"
#include "rendergraph/RenderGraph.h"
#include "shader/ShaderResourceManager.h"
#include "scene/gaussian/GaussianRenderSettings.h"
namespace Moer::Render::Raster {
class RENDER_API GaussianCompositePass {
public:
    struct RecordParameters {
        TextureWithHandle input{}, output{};
        TextureRef color{}, transmittance{};
        BufferRef status{}, ranges{};
        EGaussianDebugView debug_view = EGaussianDebugView::Color;
    };
    GaussianCompositePass(ShaderManager& manager, BindlessArrayRef bindless);
    ~GaussianCompositePass();
    RecordParameters Prepare(TextureWithHandle input, TextureRef color, TextureRef transmittance,
        BufferRef status, BufferRef ranges, EGaussianDebugView debug_view = EGaussianDebugView::Color);
    void Record(CommandList& commands, const RecordParameters& parameters);
    RenderGraph::PassHandle AddToGraph(RenderGraph& graph, RecordParameters parameters,
        RenderGraph::TextureHandle input, RenderGraph::TextureHandle color,
        RenderGraph::TextureHandle transmittance, RenderGraph::BufferHandle status,
        RenderGraph::BufferHandle ranges, RenderGraph::TextureHandle output);
    const TextureWithHandle& GetOutput() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
