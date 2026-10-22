#pragma once

#ifdef __cplusplus
#include "GaussianParameters.h"
namespace Moer::Render {
#else
#include "shared/gaussian/GaussianParameters.h"
namespace Moer {
#endif

struct GGaussianCamera {
#ifdef __cplusplus
    float4x4 world_to_view;
    float4x4 view_to_clip;
#else
    row_major float4x4 world_to_view;
    row_major float4x4 view_to_clip;
#endif
    float4 position;
    uint4  viewport; // width, height, tile columns, tile rows
    float4 filter;   // minimum pixel variance, alpha cutoff, near clip, far clip
    float4 background;
};

struct GProjectedGaussian {
    float4 center_depth;  // pixel center xy, positive view depth, conservative log-alpha cutoff
    float4 conic_opacity; // inverse covariance xx/xy/yy, activated opacity
    float4 color;
    uint4  tile_rect; // inclusive min xy, exclusive max xy
};

// Every kernel stays below the portable 128-byte push-constant limit.
struct GaussianDispatchParams {
    uint4 info; // work count, pair capacity, tile columns, tile rows
    uint4 job;  // kernel-specific indices/counts; never CPU pointers
};

#ifdef __cplusplus
static_assert(sizeof(GGaussianCamera) == 192);
static_assert(sizeof(GProjectedGaussian) == 64);
static_assert(sizeof(GaussianDispatchParams) == 32);
#endif
} // namespace Moer / Moer::Render
