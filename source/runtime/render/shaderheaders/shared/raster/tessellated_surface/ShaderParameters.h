/**
 * Shared parameters for the procedural tessellated surface showcase.
 *
 * Include shared/raster/ShaderParameters.h instead of including this file directly.
 */
#pragma once

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

struct TessellatedSurfaceData {
    float4x4 world2clip;

    // xyz: camera world position, w: tan(vertical field of view / 2)
    float4 camera_position_tan_half_fov;
    // xy: viewport size, z: target edge length in pixels, w: maximum tessellation factor
    float4 viewport_tessellation;
    // xyz: surface center, w: half extent of the square surface
    float4 surface_origin_extent;
    // x: macro amplitude, y: detail amplitude, z: macro frequency, w: detail frequency
    float4 displacement;
    // xy: normalized wind direction in XZ, z: domain-warp strength, w: normal finite-difference step
    float4 wind_and_normal;
    // rgb: low/slope color, w: perceptual roughness
    float4 color_low_roughness;
    // rgb: high/flat color, w: minimum tessellation factor
    float4 color_high_min_tessellation;
    // x: grid X, y: grid Z, z: preset (0 sand, 1 snow), w: debug mode
    uint4 grid_and_options;
};

#ifdef __cplusplus
static_assert(sizeof(TessellatedSurfaceData) == 192, "TessellatedSurfaceData layout must match HLSL");
#endif

#ifdef __cplusplus
} // namespace Moer::Render
#else
} // namespace Moer
#endif
