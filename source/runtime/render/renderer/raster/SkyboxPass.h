#pragma once

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/env_and_atmo_pass/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"

namespace Moer::Render::Raster {

class SkyboxPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(SkyboxPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(SkyboxPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class SkyboxPass {
public:
    SkyboxPass(RasterContext& context);

    void Process(RasterContext& context, const RasterConfig& ui_config, const Camera& camera);

private:
    SkyboxPipeline skybox_pipeline;
};
} // namespace Moer::Render::Raster