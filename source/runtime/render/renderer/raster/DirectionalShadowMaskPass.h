#pragma once

#include "math/Function.h"
#include "misc/MMemory.h"
#include "scene/Camera.h"
#include "scene/Material.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

namespace Moer::Render::Raster {

class DirectionalShadowMaskPassPipeline : public RasterPipeline {
public:
    DEFINE_RASTER_PIPELINE_CLASS(DirectionalShadowMaskPassPipeline);
    DEFINE_SHADER_CONSTANT_STRUCT(DirectionalShadowMaskPassBindlessParam, param);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_ARGS(bdls, param);
};

class DirectionalShadowMaskPass {
public:
    DirectionalShadowMaskPass(RasterContext& context);

    void Process(RasterContext& context, const RasterConfig& ui_config, const CameraRef& camera);

private:
    DirectionalShadowMaskPassPipeline directional_shadow_mask_pipeline;
};
} // namespace Moer::Render::Raster