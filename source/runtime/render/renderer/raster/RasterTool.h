// 提供 Raster 共用绘制数据、profiling 标签、限频日志和场景命令提交。
#pragma once

#include "RasterConfig.h"
#include "misc/STL.h"
#include "misc/Types.h"
#include "rhi/RHICommandDrawData.h"
#include "scene/GpuScene.h"

#include <source_location>
#include <string_view>

namespace Moer::Render {
class CommandQueue;
class RenderDevice;
}

namespace Moer::Render::Raster {

class RENDER_API RasterTool {
public:
    static Array<SingleDrawParam> GetFullScreenDrawDatas();

    static std::string_view GetShadowDepthPassProfileScopeName();

    static std::string_view GetGeometryPassProfileScopeName();

    static std::string_view GetGeometryCullingProfileScopeName();

    static std::string_view GetGeometryDrawProfileScopeName();

    static std::string_view GetCsmShadowCascadeMarkerName(uint32 cascade_index);

    static std::string_view GetCsmShadowCullingProfileScopeName(uint32 cascade_index);

    static std::string_view GetCsmShadowDrawProfileScopeName(uint32 cascade_index);

    static std::string_view GetPointShadowFaceMarkerName(uint32 face);

    static std::string_view GetPointShadowCullingProfileScopeName(uint32 face);

    static std::string_view GetPointShadowDrawProfileScopeName(uint32 face);

    [[nodiscard]] static bool ShouldLogDebugEverySeconds(
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
