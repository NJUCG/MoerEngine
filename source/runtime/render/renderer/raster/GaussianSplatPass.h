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
// Independent, lazy raster path. No mesh layout, bindless IDs or GBuffer ownership changes.
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
        bool                    is_unsupported_mixed = false
    );
    const TextureRef& GetColor() const;
    const TextureRef& GetTransmittance() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Moer::Render::Raster
