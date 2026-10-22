#pragma once

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

// Canonical standard-3DGS data. Rotation is normalized xyzw; SH is coefficient-major RGB.
// No training encodings (log-scale / opacity logit) reach the GPU.
struct GGaussian {
    float4 position;
    float4 scale_opacity;
    float4 rotation;
    float4 sh[16];
};

struct GGaussianInstance {
#ifdef __cplusplus
    float4x4 world_transform;
#else
    row_major float4x4 world_transform;
#endif
    float4   color_opacity;
    uint4    range; // first Gaussian, count, SH degree, visibility bits (game=1, editor=2)
};

#ifdef __cplusplus
static_assert(sizeof(GGaussian) == 304);
static_assert(sizeof(GGaussianInstance) == 96);
#endif
} // namespace Moer / Moer::Render
