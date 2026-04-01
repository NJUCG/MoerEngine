#ifndef MOER_RASTER_COMMON_HLSL
#define MOER_RASTER_COMMON_HLSL

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 clip2world) {
  float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
  float4 world_w = mul(clip2world, clip);
  float3 pos     = world_w.xyz / world_w.w;
  return pos;
}

float3 WorldPosFromDepthTexture(uint depth_tex_hdl, float2 screen_uv, float4x4 clip2world) {
    float depth = TextureHandle(depth_tex_hdl).Sample2D<float>(screen_uv);
    return WorldPosFromDepth(depth, screen_uv, clip2world);
}

#endif