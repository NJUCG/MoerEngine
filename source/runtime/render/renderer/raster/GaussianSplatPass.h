#pragma once
#include "rendergraph/RenderGraph.h"
#include "rhi/RHICommand.h"
#include "scene/camera/Camera.h"
#include "scene/gaussian/GaussianGpuScene.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/gaussian/GaussianRasterParameters.h"

namespace Moer::Render::Raster {
struct GaussianRasterSettings {
    uint tile_pair_capacity = 4u * 1024u * 1024u;
};
// Gaussian accumulation owns no frame presentation or mesh resources.
class RENDER_API GaussianSplatPass {
public:
    struct RecordParameters {
        const GaussianGpuScene* scene = nullptr;
        Camera                  camera{};
        uint2                   resolution{};
        bool                    is_editor_view = false;
        GaussianViewSettings    settings{};
        TextureRef              opaque_depth{};
        bool                    is_engine_view   = true;
        bool                    needs_reference  = false;
        BufferRef               shadow_data{};
        BindlessArrayRef        shadow_bindless{};
        Array<TextureRef>       shadow_maps{};
    };
    struct GraphResources {
        RenderGraph::TextureHandle color{}, transmittance{}, reference{}, display{};
        RenderGraph::BufferHandle  status{}, ranges{};
    };
    RecordParameters Prepare(
        const GaussianGpuScene* scene,
        const Camera&           camera,
        uint2                   resolution,
        bool                    is_editor_view,
        GaussianViewSettings    settings,
        TextureRef              opaque_depth,
        bool                    needs_reference = false
    );
    void                    Record(CommandList& commands, const RecordParameters& parameters);
    GraphResources          ImportGraphResources(RenderGraph& graph) const;
    RenderGraph::PassHandle AddToGraph(
        RenderGraph&               graph,
        RecordParameters           parameters,
        GraphResources             resources,
        RenderGraph::TextureHandle opaque_depth,
        RenderGraph::TokenHandle   shadow_maps = {}
    );
    const TextureRef& GetDisplay() const;
    const BufferRef&  GetRanges() const;
    explicit GaussianSplatPass(ShaderManager& manager, GaussianRasterSettings settings = {});
    ~GaussianSplatPass();
    // Standalone reference/validation helper. Frame rendering uses prepared parameters.
    TextureRef Record(
        CommandList&            commands,
        const GaussianGpuScene* scene,
        const Camera&           camera,
        uint2                   resolution,
        bool                    is_editor_view,
        bool                    is_invalid_scene = false,
        GaussianViewSettings    view_settings    = {}
    );
    // Accumulation and reference display are separate recording operations.
    TextureRef RecordAccumulation(
        CommandList&            commands,
        const GaussianGpuScene* scene,
        const Camera&           camera,
        uint2                   resolution,
        bool                    is_editor_view,
        bool                    is_invalid      = false,
        GaussianViewSettings    settings        = {},
        TextureRef              opaque_depth    = {},
        bool                    is_engine_view  = false,
        bool                    needs_reference = false,
        BufferRef               shadow_data     = {},
        BindlessArrayRef        shadow_bindless = {}
    );
    TextureRef           RecordDisplay(CommandList& commands, GaussianViewSettings settings = {});
    // Premultiplied source RGB, before layer decoding in GaussianCompositePass.
    const TextureRef&    GetColor() const;
    const TextureRef&    GetTransmittance() const;
    GaussianRasterStatus GetStatus() const;
    uint                 GetPairCapacity() const;
    void                 SetPairCapacity(uint capacity);
    BufferView           GetStatusBuffer() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Moer::Render::Raster
