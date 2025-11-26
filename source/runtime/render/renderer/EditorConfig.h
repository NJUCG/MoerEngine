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

    float            camera_speed = 25.f;
    float            camera_fovy  = 60.f;
    float            aspect_ratio = 1.f;
    SharedPtr<uint2> resolution; // hold ownership

    RasterConfig     raster_config;
    RaytracingConfig raytracing_config;
};

} // namespace Moer