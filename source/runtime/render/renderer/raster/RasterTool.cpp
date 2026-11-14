// 实现各独立 Raster Pass 共用的小型工具函数。

#include "RasterTool.h"
#include "log/LogSystem.h"
#include "misc/Timer.h"
#include "misc/Types.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/executor/RHIExecutor.h"
#include "scene/Scene.h"

#include <cassert>
#include <source_location>
#include <sstream>
#include <stdexcept>

namespace Moer::Render::Raster {

namespace {
constexpr StaticArray<std::string_view, CSM_MAX_CASCADES> s_csm_shadow_cascade_marker_names = {
    "CSM Cascade 0",
    "CSM Cascade 1",
    "CSM Cascade 2",
    "CSM Cascade 3"
};

constexpr StaticArray<std::string_view, CSM_MAX_CASCADES> s_csm_shadow_culling_scope_names = {
    "Raster Shadow Culling CSM0",
    "Raster Shadow Culling CSM1",
    "Raster Shadow Culling CSM2",
    "Raster Shadow Culling CSM3"
};

constexpr StaticArray<std::string_view, CSM_MAX_CASCADES> s_csm_shadow_draw_scope_names = {
    "Raster Shadow Draw CSM0",
    "Raster Shadow Draw CSM1",
    "Raster Shadow Draw CSM2",
    "Raster Shadow Draw CSM3"
};

constexpr StaticArray<std::string_view, CUBE_FACE_Num> s_point_shadow_face_marker_names = {
    "Point Shadow Face +X",
    "Point Shadow Face -X",
    "Point Shadow Face +Y",
    "Point Shadow Face -Y",
    "Point Shadow Face +Z",
    "Point Shadow Face -Z"
};

constexpr StaticArray<std::string_view, CUBE_FACE_Num> s_point_shadow_culling_scope_names = {
    "Raster PointShadow Culling +X",
    "Raster PointShadow Culling -X",
    "Raster PointShadow Culling +Y",
    "Raster PointShadow Culling -Y",
    "Raster PointShadow Culling +Z",
    "Raster PointShadow Culling -Z"
};

constexpr StaticArray<std::string_view, CUBE_FACE_Num> s_point_shadow_draw_scope_names = {
    "Raster PointShadow Draw +X",
    "Raster PointShadow Draw -X",
    "Raster PointShadow Draw +Y",
    "Raster PointShadow Draw -Y",
    "Raster PointShadow Draw +Z",
    "Raster PointShadow Draw -Z"
};

static_assert(s_csm_shadow_cascade_marker_names.size() == CSM_MAX_CASCADES);
static_assert(s_csm_shadow_culling_scope_names.size() == CSM_MAX_CASCADES);
static_assert(s_csm_shadow_draw_scope_names.size() == CSM_MAX_CASCADES);
static_assert(s_point_shadow_face_marker_names.size() == CUBE_FACE_Num);
static_assert(s_point_shadow_culling_scope_names.size() == CUBE_FACE_Num);
static_assert(s_point_shadow_draw_scope_names.size() == CUBE_FACE_Num);

} // namespace

// 返回屏幕空间全屏三角形绘制共用的 draw 参数。
Array<SingleDrawParam> RasterTool::GetFullScreenDrawDatas() {
    Array<SingleDrawParam> draw_data;
    draw_data.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
    return draw_data;
}

// 返回整个阴影深度阶段使用的 profiling scope 名称。
std::string_view RasterTool::GetShadowDepthPassProfileScopeName() {
    return "Raster ShadowDepthPass";
}

// 返回整个几何阶段使用的 profiling scope 名称。
std::string_view RasterTool::GetGeometryPassProfileScopeName() {
    return "Raster GeometryPass";
}

// 返回主相机视角几何剔除 dispatch 使用的 profiling scope 名称。
std::string_view RasterTool::GetGeometryCullingProfileScopeName() {
    return "Raster Geometry Culling";
}

// 返回几何阶段绘制提交使用的 profiling scope 名称。
std::string_view RasterTool::GetGeometryDrawProfileScopeName() {
    return "Raster Geometry Draw";
}

// 返回某个级联阴影的可视 marker 名称。
std::string_view RasterTool::GetCsmShadowCascadeMarkerName(uint32 cascade_index) {
    assert(cascade_index < CSM_MAX_CASCADES);
    return s_csm_shadow_cascade_marker_names[cascade_index];
}

// 返回某个级联阴影视锥剔除 dispatch 使用的 profiling scope 名称。
std::string_view RasterTool::GetCsmShadowCullingProfileScopeName(uint32 cascade_index) {
    assert(cascade_index < CSM_MAX_CASCADES);
    return s_csm_shadow_culling_scope_names[cascade_index];
}

std::string_view RasterTool::GetCsmShadowDrawProfileScopeName(uint32 cascade_index) {
    assert(cascade_index < CSM_MAX_CASCADES);
    return s_csm_shadow_draw_scope_names[cascade_index];
}

std::string_view RasterTool::GetPointShadowFaceMarkerName(uint32 face) {
    assert(face < CUBE_FACE_Num);
    return s_point_shadow_face_marker_names[face];
}

std::string_view RasterTool::GetPointShadowCullingProfileScopeName(uint32 face) {
    assert(face < CUBE_FACE_Num);
    return s_point_shadow_culling_scope_names[face];
}

std::string_view RasterTool::GetPointShadowDrawProfileScopeName(uint32 face) {
    assert(face < CUBE_FACE_Num);
    return s_point_shadow_draw_scope_names[face];
}

bool RasterTool::ShouldLogDebugEverySeconds(
    double               seconds,
    std::source_location location
) {
#if SPDLOG_ACTIVE_LEVEL > SPDLOG_LEVEL_DEBUG
    (void)seconds;
    (void)location;
    return false;
#else
    const auto* logger = spdlog::default_logger_raw();
    if (!logger->should_log(spdlog::level::debug) && !logger->should_backtrace()) {
        return false;
    }
    if (seconds <= 0.0) {
        return true;
    }

    static UnorderedMap<std::string, LoopedTimer> s_debug_log_timers;

    std::ostringstream site_key;
    site_key << location.file_name() << ':' << location.line();
    auto timer_it = s_debug_log_timers.try_emplace(site_key.str(), seconds, true).first;
    return timer_it->second.Tick();
#endif
}

// 执行 Scene 同步阶段积累的 copy/gfx command list。
void RasterTool::ExecuteScenePendingCommands(
    GpuScene::PendingCommandList&& commands,
    RenderDevice&                  device,
    CommandQueue&                  gfx_queue
) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyRenderThread());
    (void)gfx_queue;

    auto accepted = device.CreateFence();
    commands.Signal(accepted, 1);
    const bool modern_graphics_profiling =
        commands.HasGpuScopeRecorder() || commands.IsLegacyGpuProfilingSuppressedForGeneration();
    Array<RHISubmissionRequest>           submissions{};
    CmdSubmit                             graphics_submit = commands.Submit();
    if (!modern_graphics_profiling) {
        graphics_submit.TickProfiling();
    }
    submissions.emplace_back(
        EQueueType::Graphics, std::move(graphics_submit)
    );
    RHIExecutor::Get().Submit(
        std::move(submissions),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!accepted->WaitSubmitted(1)) {
        throw std::runtime_error("Compiled scene update was rejected before native submission");
    }
}

} // namespace Moer::Render::Raster
