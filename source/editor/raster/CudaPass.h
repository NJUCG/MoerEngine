#pragma once

#include <cuda_runtime.h>

#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterResource.h"
#include "RasterTool.h"
#include "ui/raster_ui/RasterConfig.h"

namespace Moer::Render::Raster {

/**
 * MARK: CUDA Pass
 */
class CudaPass {
public:
    CudaPass(RasterContext& context) {}

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {
        if (ui_config.ai_is_cuda_enabled == false) return input_image;

        return input_image;
    }

private:
};

} // namespace Moer::Render::Raster