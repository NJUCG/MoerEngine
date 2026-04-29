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

    static void ProcessRenderableTransformMotion(RasterConfig& raster_config, Scene& scene, uint64 frame_index);

    static void ProcessPointLightTransformMotion(RasterConfig& raster_config, Scene& scene, uint64 frame_index);
};

} // namespace Moer::Render::Raster
