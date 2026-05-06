#pragma once

#include "misc/STL.h"
#include "RasterConfig.h"
#include "rhi/RHICommandDrawData.h"
#include "string/String.h"

#include <source_location>

namespace Moer::Render {
class CommandQueue;
}

namespace Moer::Render::Raster {

class RasterTool {
public:
    static Array<SingleDrawParam> GetFullScreenDrawDatas();

    static StringView GetShadowDepthPassProfileScopeName();

    static StringView GetGeometryPassProfileScopeName();

    static StringView GetGeometryCullingProfileScopeName();

    static StringView GetGeometryDrawProfileScopeName();

    static StringView GetShadowCullingProfileScopeName(uint cascade_index);

    static StringView GetShadowDrawProfileScopeName(uint cascade_index);

    static void LogDebugEverySeconds(
        std::string_view      str,
        double                seconds,
        std::source_location  location = std::source_location::current()
    );

    static void TickAndLogProfiling(CommandQueue& gfx_queue, const RasterConfig& raster_config);
};

} // namespace Moer::Render::Raster