#pragma once

#include "misc/STL.h"
#include "RasterConfig.h"
#include "rhi/RHICommandDrawData.h"

#include <string_view>

namespace Moer::Render {
class CommandQueue;
}

namespace Moer::Render::Raster {

class RasterTool {
public:
    static Array<SingleDrawParam> GetFullScreenDrawDatas();

    static std::string_view GetShadowDepthPassProfileScopeName();

    static std::string_view GetGeometryPassProfileScopeName();

    static std::string_view GetGeometryCullingProfileScopeName();

    static std::string_view GetGeometryDrawProfileScopeName();

    static std::string_view GetShadowCullingProfileScopeName(uint cascade_index);

    static std::string_view GetShadowDrawProfileScopeName(uint cascade_index);

    static void TickAndLogProfiling(CommandQueue& gfx_queue, const RasterConfig& raster_config);
};

} // namespace Moer::Render::Raster