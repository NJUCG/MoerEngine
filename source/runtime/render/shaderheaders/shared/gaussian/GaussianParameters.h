#pragma once

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
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
    float4x4 world_transform;
    float4   color_opacity;
    uint4    range; // first Gaussian, count, SH degree, reserved
};

#ifdef __cplusplus
static_assert(sizeof(GGaussian) == 304);
static_assert(sizeof(GGaussianInstance) == 96);
} // namespace Moer::Render
#endif
