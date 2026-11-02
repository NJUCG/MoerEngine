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
    float4x4 world_transform;
    float4   color_opacity;
    uint4
        range; // first Gaussian, count, SH degree, flags: game=1, editor=2, cast shadows=4, receive shadows=8
};

#ifdef __cplusplus
static_assert(
    sizeof(GGaussian) == 304,
    "Changing the raw .moca Gaussian record requires a cache version migration"
);

#endif
} // namespace Moer / Moer::Render
