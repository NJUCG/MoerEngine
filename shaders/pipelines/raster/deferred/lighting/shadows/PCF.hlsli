#ifndef MOER_LIGHTING_SHADOWS_PCF_HLSLI
#define MOER_LIGHTING_SHADOWS_PCF_HLSLI

#include "core/common/Bindless.hlsl"
#include "pipelines/raster/deferred/lighting/shadows/ShadowCore.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/ShadowSampling.hlsli"

// 基础的 PCF 采样
float calculate_pcf(uint shadow_map_handle, float2 uv, float fragment_depth, float radius_uv) {
    if (radius_uv <= 0.0) return 1.0;
    
    float shadow_contribution = 0.0;
    // 这里的循环次数 16 也可以考虑用宏控制，比如 #if HIGH_QUALITY_SHADOW
    [unroll]
    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i] * radius_uv;
        float occluder_depth = TextureHandle(shadow_map_handle).Sample2D<float>(uv + offset).x;
        if (occluder_depth > fragment_depth + SHADOW_BIAS) {
            shadow_contribution += 1.0;
        }
    }
    return 1.0 - (shadow_contribution / 16.0);
}

// 基础 PCF (带随机旋转)
float calculate_pcf(ShadowContext ctx, float radius_uv) {
    if (radius_uv <= 0.0) return 1.0;
    
    float shadow_contribution = 0.0;
    // 获取随机旋转矩阵
    float2x2 rotation = GetRandomRotation(ctx.screenUV);

    [unroll]
    for (int i = 0; i < 16; ++i) {
        // 应用旋转
        float2 offset = mul(rotation, POISSON_DISK_16[i]) * radius_uv;
        
        float occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(ctx.shadowUV + offset).x;
        
        // Reverse-Z 逻辑: occluder > fragment 代表更近 (遮挡)
        if (occluder_depth > ctx.fragmentDepth + SHADOW_BIAS) {
            shadow_contribution += 1.0;
        }
    }
    
    return 1.0 - (shadow_contribution / 16.0);
}

#endif