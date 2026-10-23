#pragma once
#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif
// Immutable per-frame light views, shared by casting and receiving.
struct GaussianShadowData {
#ifdef __cplusplus
    float4x4 world_to_clip[6];
    float4x4 world_to_view;
#else
    row_major float4x4 world_to_clip[6];
    row_major float4x4 world_to_view;
#endif
    float4 splits;
    float4 blends;
    float4 light;         // direction towards a directional light, or point-light position
    uint4  gaussian_maps; // four cascades; cube uses x
    uint4  mesh_maps;
    uint4  config;  // mode: 0=invalid, 1=CSM, 2=cube; cascade count; resolution; cast enabled
    float4 options; // alpha cutoff, receive strength, world bias, blend enabled
    float4 clips;   // light near/far, view near/far
};
struct GaussianShadowDraw {
#ifdef __cplusplus
    float4x4 world_to_clip;
#else
    row_major float4x4 world_to_clip;
#endif
    uint4  info;    // first gaussian, instance index, resolution, reserved
    float4 options; // cutoff
};
#ifdef __cplusplus
static_assert(sizeof(GaussianShadowData) == 576);
static_assert(sizeof(GaussianShadowDraw) == 96);
#endif
}
