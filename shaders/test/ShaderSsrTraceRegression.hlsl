// Exercise the production trace with controlled rays, depth and projection.
#define main SsrPixelMain
#include "pipelines/postprocess/lighting_effects/Ssr.hlsl"
#undef main

float4 main(float4 position : SV_POSITION) : SV_TARGET {
    float2 hit_uv;
    bool hit = TraceScreenSpaceReflection(
        moer_constants.param.camera_position,
        float3(moer_constants.param.ssr_step_base, 0.f, 0.f),
        0.f,
        hit_uv
    );
    return float4(hit ? 1.f : 0.f, hit_uv, 0.f);
}
