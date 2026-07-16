// Builds the screen-space directional shadow factor consumed by deferred lighting.
#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterResource.h"

namespace Moer::Render::Raster {

class DirectionalShadowMaskPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(DirectionalShadowMaskPassPipeline);
    DEFINE_SHADER_BUFFER(lighting_data);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(DirectionalShadowMaskPassBindlessParam, param);
    DEFINE_SHADER_ARGS(lighting_data, bdls, param);
};

class DirectionalShadowMaskPass {
public:
    DirectionalShadowMaskPass(RasterContext& context);

    void Process(RasterContext& context);

private:
    DirectionalShadowMaskPassPipeline pipeline;
};
} // namespace Moer::Render::Raster
