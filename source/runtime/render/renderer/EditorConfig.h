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

enum class EEditorViewportMode {
    Scene,
    Game,
};
constexpr std::string_view k_editor_viewport_mode_names[] = {"Scene", "Game"};

inline float4 GetCsmGizmoCascadeColor(uint cascade_index) {
    if (cascade_index == 0u) {
        return float4(1.00f, 0.30f, 0.05f, 0.96f);
    }
    if (cascade_index == 1u) {
        return float4(0.30f, 1.00f, 0.38f, 0.94f);
    }
    if (cascade_index == 2u) {
        return float4(0.92f, 0.28f, 1.00f, 0.92f);
    }
    return float4(1.00f, 0.88f, 0.10f, 0.90f);
}

struct SceneViewGizmoConfig {
    bool enabled = true;

    bool show_main_camera = true;

    bool show_csm                  = true;
    bool show_csm_split_frustums   = true;
    bool show_csm_bounding_spheres = true;
    uint csm_cascade_visibility_mask = (1u << CSM_MAX_CASCADES) - 1u;

    bool show_probe_gi                = false;
    bool show_probe_gi_probes         = true;
    bool show_probe_gi_volume_bounds  = true;
    bool show_probe_gi_adaptive_cells = false;

    float line_thickness = 0.004f;
};

struct EditorConfig {
    ERenderMethod selected_render_method = ERenderMethod::Raster;
    std::string   scene_path             = "";
    EEditorViewportMode active_viewport_mode = EEditorViewportMode::Game;
    SceneViewGizmoConfig scene_view_gizmos;

    bool  camera_projection_override_enabled = true;
    float camera_speed_log10                 = log10f(25.f);
    float camera_fovy                        = 60.f;
    float camera_far_clip_log10              = 3.f;
    float camera_near_clip_log10             = -2.f;

    RasterConfig        raster_config;
    RaytracingConfig    raytracing_config;
    SceneTestCaseConfig scene_test_case_config;

    // 为了避免数据不一致，这里对resolution进行封装。引擎中必须优先保证该struct中的resolution是正确的
private:
    uint2 resolution;

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
};

} // namespace Moer
