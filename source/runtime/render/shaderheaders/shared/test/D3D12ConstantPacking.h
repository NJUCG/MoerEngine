#pragma once
#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif
struct D3D12PackingData {
    uint index;
    float3 exposure;
    float3 camera;
    float4x4 transform;
    uint samples[3];
};
}
