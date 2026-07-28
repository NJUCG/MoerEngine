#pragma once

#include "misc/Traits.h"

#include "raster/RasterConfig.h"
#include "raytracing/RaytracingConfig.h"
#include "scene/testcase/SceneTestCaseConfig.h"

namespace Moer {

enum class ERenderMethod {
    Raster,
    Raytracing,
};
constexpr std::string_view k_render_method_names[] = {"Raster", "Raytracing"};

struct EditorConfig {
    ERenderMethod selected_render_method = ERenderMethod::Raster;
    std::string   scene_path             = "";

    bool  camera_projection_override_enabled = true;
    float camera_speed_log10     = log10f(25.f);
    float camera_fovy            = 60.f;
    float camera_far_clip_log10  = 3.f;
    float camera_near_clip_log10 = -2.f;

    RasterConfig        raster_config;
    RaytracingConfig    raytracing_config;
    SceneTestCaseConfig scene_test_case_config;

    // Play mode states
    bool play_mode_enabled       = false;
    bool play_mode_capture_input = false;

    // 为了避免数据不一致，这里对resolution进行封装。引擎中必须优先保证该struct中的resolution是正确的
    // resolution: 窗口/交换链尺寸；render_resolution: 实际场景渲染尺寸（即 SceneColor viewport 尺寸）
private:
    uint2 resolution;
    uint2 render_resolution;

public:
    void SetResolution(uint2 _resolution) {
        resolution = _resolution;
    }
    void SetResolution(uint _width, uint _height) {
        resolution = uint2(_width, _height);
    }
    const uint2& GetResolution() const {
        return resolution;
    }
    uint2& GetResolution() {
        return resolution;
    }

    void SetRenderResolution(uint2 _render_resolution) {
        render_resolution = _render_resolution;
    }
    void SetRenderResolution(uint _width, uint _height) {
        render_resolution = uint2(_width, _height);
    }
    const uint2& GetRenderResolution() const {
        return render_resolution;
    }
    uint2& GetRenderResolution() {
        return render_resolution;
    }
};

} // namespace Moer
