#ifndef RASTER_SKYBOX_HLSLI
#define RASTER_SKYBOX_HLSLI

#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::SkyboxPassBindlessParam> param;

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 inv_view_proj) {
    float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
    float4 world_w = mul(inv_view_proj, clip);
    float3 pos     = world_w.xyz / world_w.w;
    return pos;
}

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    float3 in_pos       = WorldPosFromDepth(0.99, in_uv, param.inv_view_proj);
    float3 view_dir     = normalize(in_pos - param.camera_pos);
    float3 skybox_color = TextureHandle(param.cubemap_handle).SampleCube<float3>(view_dir);
    skybox_color *= param.exposure_factor;
    return float4(skybox_color, 1.0);
}

#endif // RASTER_SKYBOX_HLSLI