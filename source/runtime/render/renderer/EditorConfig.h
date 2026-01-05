#pragma once

#include "Core.h"
#include "misc/MMemory.h"
#include "misc/Traits.h"

#include "raster/RasterConfig.h"
#include "raytracing/RaytracingConfig.h"

namespace Moer {

enum class ERenderMethod {
    Raster,
    Raytracing,
};
constexpr std::string_view k_render_method_names[] = {"Raster", "Raytracing"};

struct EditorConfig {
    ERenderMethod selected_render_method = ERenderMethod::Raster;
    std::string   scene_path             = "";

    float camera_speed_log10     = log10f(25.f);
    float camera_fovy            = 60.f;
    float aspect_ratio           = 1.f;
    float camera_far_clip_log10  = 3.f;
    float camera_near_clip_log10 = -2.f;

    RasterConfig     raster_config;
    RaytracingConfig raytracing_config;

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