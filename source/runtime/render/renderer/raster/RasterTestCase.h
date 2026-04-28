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
};

} // namespace Moer::Render::Raster