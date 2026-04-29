#pragma once

#include "RasterConfig.h"

namespace Moer {
class Scene;
}

namespace Moer::Render::Raster {

class RasterTestCase {
public:
    static void ProcessDebugSceneUpdateRequest(RasterConfig& raster_config, Scene& scene);

    static bool ProcessDebugMaterialRequest(RasterConfig& raster_config, Scene& scene);

    static void ProcessRenderableTransformMotion(RasterConfig& raster_config, Scene& scene, float elapsed_time_seconds);

    static void ProcessPointLightTransformMotion(RasterConfig& raster_config, Scene& scene, float elapsed_time_seconds);
};

} // namespace Moer::Render::Raster
