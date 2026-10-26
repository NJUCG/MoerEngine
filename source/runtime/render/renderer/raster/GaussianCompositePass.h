#pragma once
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include "scene/gaussian/GaussianRenderSettings.h"
#include "shader/ShaderResourceManager.h"
namespace Moer::Render::Raster {
class RENDER_API GaussianCompositePass {
public:
    struct RecordParameters {
        TextureWithHandle  input{}, output{};
        TextureRef         color{}, transmittance{};
        BufferRef          status{}, ranges{};
        EGaussianDebugView debug_view = EGaussianDebugView::Color;
        bool               is_shadow_depth_valid = false;
    };
    GaussianCompositePass(ShaderManager& manager, BindlessArrayRef bindless);
    ~GaussianCompositePass();
    RecordParameters Prepare(
        TextureWithHandle  input,
        TextureRef         color,
        TextureRef         transmittance,
        BufferRef          status,
        BufferRef          ranges,
        EGaussianDebugView debug_view = EGaussianDebugView::Color
    );
    void                    Record(CommandList& commands, const RecordParameters& parameters);
    RenderGraph::PassHandle AddToGraph(
        RenderGraph&               graph,
        RecordParameters           parameters,
        RenderGraph::TextureHandle input,
        RenderGraph::TextureHandle color,
        RenderGraph::TextureHandle transmittance,
        RenderGraph::BufferHandle  status,
        RenderGraph::BufferHandle  ranges,
        RenderGraph::TextureHandle output
    );
    void   RecordDebug(CommandList& commands, const RecordParameters& parameters, TextureRef output);
    double GetGpuTime();
    const TextureWithHandle& GetOutput() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Moer::Render::Raster
