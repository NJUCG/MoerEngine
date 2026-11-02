#pragma once

#ifdef __cplusplus
#include "GaussianParameters.h"
#include "GaussianViewModes.h"
namespace Moer::Render {
#else
#include "shared/gaussian/GaussianParameters.h"
#include "shared/gaussian/GaussianViewModes.h"
namespace Moer {
#endif

struct GGaussianCamera {
    float4x4 world_to_view;
    float4x4 view_to_clip;
    float4x4 clip_to_view;
    float4   position;
    uint4    viewport; // width, height, tile columns, tile rows
    float4   filter;   // minimum pixel variance, alpha cutoff, near clip, far clip
    float4   background;
};

struct GProjectedGaussian {
    float4 center_depth;  // pixel center xy, positive view depth, conservative log-alpha cutoff
    float4 conic_opacity; // inverse covariance xx/xy/yy, activated opacity
    float4 color;         // display RGB and Mesh shadow visibility (1 when disabled)
    uint4  tile_rect;     // inclusive min xy, exclusive max xy
};

// Every kernel stays below the portable 128-byte push-constant limit.
struct GaussianDispatchParams {
    uint4 info; // work count, pair capacity, tile columns, tile rows
    uint4 job;  // kernel-specific indices/counts; never CPU pointers
};

// The accumulated layer remains in the source display RGB domain until compositing.
struct GaussianCompositeParams {
    uint4 viewport; // width, height, tile columns, debug view
    float brightness_gain;
    uint  is_visibility_debug;
    uint2 reserved;
};

#ifdef __cplusplus
static_assert(sizeof(GaussianCompositeParams) <= 128, "Push constants must fit the portable 128-byte limit");
static_assert(sizeof(GaussianDispatchParams) <= 128, "Push constants must fit the portable 128-byte limit");
#endif
} // namespace Moer / Moer::Render
