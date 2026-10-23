#pragma once
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
    explicit GaussianSplatPass(ShaderManager& manager, GaussianRasterSettings settings = {});
    ~GaussianSplatPass();
    TextureRef Record(
        CommandList&            commands,
        const GaussianGpuScene* scene,
        const Camera&           camera,
        uint2                   resolution,
        bool                    is_editor_view,
        bool                    is_unsupported_mixed = false,
        GaussianViewSettings    view_settings        = {}
    );
    // Accumulation and reference display are separate recording operations.
    TextureRef RecordAccumulation(CommandList& commands, const GaussianGpuScene* scene,
        const Camera& camera, uint2 resolution, bool is_editor_view,
        bool is_invalid = false, GaussianViewSettings settings = {});
    TextureRef RecordDisplay(CommandList& commands, GaussianViewSettings settings = {});
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
