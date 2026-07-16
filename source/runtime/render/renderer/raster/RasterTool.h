// 提供 Raster 共用绘制数据、profiling 标签、限频日志和场景命令提交。
#pragma once

#include "misc/STL.h"
#include "RasterConfig.h"
#include "rhi/RHICommandDrawData.h"
#include "scene/GpuScene.h"

#include <source_location>
#include <string_view>

namespace Moer::Render {
class CommandQueue;
class RenderDevice;
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

    static void LogDebugEverySeconds(
        std::string_view      message,
        double                seconds,
        std::source_location  location = std::source_location::current()
    );

    static void TickAndLogProfiling(CommandQueue& gfx_queue, const RasterConfig& raster_config);

    static void ExecuteScenePendingCommands(
        GpuScene::PendingCommandList&& commands,
        RenderDevice&                  device,
        CommandQueue&                  gfx_queue
    );
};

} // namespace Moer::Render::Raster
