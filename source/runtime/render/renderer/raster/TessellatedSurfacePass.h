#pragma once

#include "RasterConfig.h"
#include "RasterResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/tessellated_surface/ShaderParameters.h"

namespace Moer::Render::Raster {

class TessellatedSurfacePipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(TessellatedSurfacePipeline);
    DEFINE_SHADER_BUFFER(surface_data);
    DEFINE_SHADER_ARGS(surface_data);
};

class TessellatedSurfacePass {
public:
    explicit TessellatedSurfacePass(RasterContext& context);

    void Process(RasterContext& context, const RasterConfig& config, const Camera& camera);

    bool IsSupported() const {
        return supported;
    }

private:
    TessellatedSurfacePipeline pipeline;
    BufferRef                  surface_data_buffer;
    uint32_t                   device_max_tessellation_factor = 0;
    bool                       supported                      = false;
};

} // namespace Moer::Render::Raster
