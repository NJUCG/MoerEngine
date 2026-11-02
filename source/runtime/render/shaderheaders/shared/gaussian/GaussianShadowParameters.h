#pragma once
#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif
// Immutable per-frame light views, shared by casting and receiving.
struct GaussianShadowData {
    float4x4 world_to_clip[6];
    float4x4 world_to_view;
    float4   splits;
    float4   blends;
    float4   light;         // direction towards a directional light, or point-light position
    uint4    gaussian_maps; // four cascades; cube uses x
    uint4    mesh_maps;
    uint4    config;        // mode: 0=invalid, 1=CSM, 2=cube; cascade count; resolution; cast enabled
    uint4    light_indices; // selected GPU slots: directional, point; legacy Mesh cube V flip; reserved
    float4   options;       // alpha cutoff, receive strength, world bias, blend enabled
    float4   clips;         // light near/far, view near/far
};
struct GaussianShadowDraw {
    float4x4 world_to_clip;
    uint4    info;    // first gaussian, instance index, resolution, reserved
    float4   options; // cutoff
};
#ifdef __cplusplus
static_assert(sizeof(GaussianShadowDraw) <= 128, "Push constants must fit the portable 128-byte limit");
#endif
}
