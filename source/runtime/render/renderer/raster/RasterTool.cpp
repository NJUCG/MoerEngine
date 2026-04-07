#include "RasterTool.h"

#include "RasterConfig.h"
#include "log/LogSystem.h"
#include "misc/Timer.h"
#include "rhi/RHICommand.h"

#include <cassert>
#include <span>
#include <sstream>

namespace Moer::Render::Raster {

namespace {
constexpr StaticArray<std::string_view, CSM_MAX_CASCADES> s_shadow_culling_scope_names = {
    "Raster Shadow Culling CSM0",
    "Raster Shadow Culling CSM1",
    "Raster Shadow Culling CSM2",
    "Raster Shadow Culling CSM3"
};

constexpr StaticArray<std::string_view, CSM_MAX_CASCADES> s_shadow_draw_scope_names = {
    "Raster Shadow Draw CSM0",
    "Raster Shadow Draw CSM1",
    "Raster Shadow Draw CSM2",
    "Raster Shadow Draw CSM3"
};
} // namespace

// 返回屏幕空间全屏三角形绘制共用的 draw 参数。
Array<SingleDrawParam> RasterTool::GetFullScreenDrawDatas() {
    Array<SingleDrawParam> full_screen_draw_datas;
    full_screen_draw_datas.emplace_back(SingleDrawParam{3, 1, 0, 0, 0});
    return full_screen_draw_datas;
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

// 返回某个级联阴影视锥剔除 dispatch 使用的 profiling scope 名称。
std::string_view RasterTool::GetShadowCullingProfileScopeName(uint cascade_index) {
    assert(cascade_index < CSM_MAX_CASCADES);
    return s_shadow_culling_scope_names[cascade_index];
}

// 返回某个级联阴影绘制提交使用的 profiling scope 名称。
std::string_view RasterTool::GetShadowDrawProfileScopeName(uint cascade_index) {
    assert(cascade_index < CSM_MAX_CASCADES);
    return s_shadow_draw_scope_names[cascade_index];
}

// 按固定时间间隔输出 raster GPU profiling 汇总，避免把 profiler 状态放进 renderer。
void RasterTool::TickAndLogProfiling(CommandQueue& gfx_queue, const RasterConfig& raster_config) {
    static LoopedTimer s_profiling_log_timer(2.0, false);
    if (!s_profiling_log_timer.Tick()) {
        return;
    }

    const auto profile_data = gfx_queue.GetProfilerEntry();
    if (profile_data.gpu_entries.empty()) {
        return;
    }

    auto find_gpu_time = [&profile_data](std::string_view name) -> double {
        for (const auto& entry : profile_data.gpu_entries) {
            if (entry.name == name) {
                return entry.time;
            }
        }
        return -1.0;
    };

    auto sum_gpu_times = [&find_gpu_time](std::span<const std::string_view> names) -> double {
        double total = 0.0;
        for (std::string_view name : names) {
            const double time = find_gpu_time(name);
            if (time >= 0.0) {
                total += time;
            }
        }
        return total;
    };

    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(3);

    const double graphics_exec      = find_gpu_time("Graphics Exec");
    const double shadow_culling_sum = sum_gpu_times(s_shadow_culling_scope_names);
    const double shadow_draw_sum    = sum_gpu_times(s_shadow_draw_scope_names);
    const double geometry_culling   = find_gpu_time(GetGeometryCullingProfileScopeName());
    const double geometry_draw      = find_gpu_time(GetGeometryDrawProfileScopeName());

    stream << "[RasterProfile] GPU(ms)";
    if (graphics_exec >= 0.0) {
        stream << " GraphicsExec=" << graphics_exec;
    }
    if (shadow_culling_sum > 0.0) {
        stream << " ShadowCullSum=" << shadow_culling_sum;
    }
    if (shadow_draw_sum > 0.0) {
        stream << " ShadowDrawSum=" << shadow_draw_sum;
    }
    if (geometry_culling >= 0.0) {
        stream << " GeometryCull=" << geometry_culling;
    }
    if (geometry_draw >= 0.0) {
        stream << " GeometryDraw=" << geometry_draw;
    }

    const auto& culling_stats = raster_config.culling_stats;
    if (raster_config.enable_frustum_culling && culling_stats.total_instances_before > 0) {
        stream << " | CullStats inst=" << culling_stats.total_instances_after << "/"
               << culling_stats.total_instances_before << " draws=" << culling_stats.visible_draws << "/"
               << culling_stats.total_draws << " ratio=" << culling_stats.GetCullingRatio() * 100.0f << "%";
    }

    LOG_INFO("{}", stream.str());
}

} // namespace Moer::Render::Raster