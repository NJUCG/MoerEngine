#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "core/common/Common.hlsl"
#include "pipelines/raster/deferred/lighting/shadows/Shadows.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::DirectionalShadowMaskPassBindlessParam> param;

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 inv_view_proj) {
    float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
    float4 world_w = mul(inv_view_proj, clip);
    float3 pos     = world_w.xyz / world_w.w;
    return pos;
}

float main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: Lighting Data
    ArrayBuffer        global_params = ArrayBuffer(param.global_param_hdl);
    Moer::LightingData lighting_data = global_params.Load<Moer::LightingData>(0);

    // MARK: GBuffer
    float  depth  = TextureHandle(param.depth_hdl).Sample2D<float>(in_uv);
    float3 normal = normalize(Raster::UnpackNormal(TextureHandle(param.normal_hdl).Sample2D<float3>(in_uv)));
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.inv_view_proj);

    float shadow = calculate_shadow(lighting_data, position, in_uv, normal);

    return shadow;
}