#ifndef MOER_LIGHTING_SHADOWS_PCSS_HLSLI
#define MOER_LIGHTING_SHADOWS_PCSS_HLSLI

#include "pipelines/raster/deferred/lighting/shadows/PCF.hlsli"

// 1. Blocker Search
// 参数只剩 ctx 和 search_radius，非常清爽
float find_blocker(ShadowContext ctx, float search_radius_uv) {
    uint num_blockers = 0;
    float avg_blocker_depth = 0.0;
    
    float2x2 rotation = GetRandomRotation(ctx.screenUV);

    float dynamicBias = GetSlopeScaledBias(ctx.normal, ctx.lightDir);

    [unroll]
    for (int i = 0; i < 16; ++i) {
        float2 offset = mul(rotation, POISSON_DISK_16[i]) * search_radius_uv;
        float occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(ctx.shadowUV + offset).x;
        
        // Reverse-Z 逻辑
        if (occluder_depth > ctx.fragmentDepth + SHADOW_BIAS) {
        //if (occluder_depth > ctx.fragmentDepth + dynamicBias) {
            avg_blocker_depth += occluder_depth;
            num_blockers++;
        }
    }
    
    if (num_blockers == 0) return -1.0;
    return avg_blocker_depth / float(num_blockers);
}

// 2. Penumbra Size
// 保留你的特定公式
float calculate_penumbra(ShadowContext ctx, float avg_blocker_depth) {
    // 你的公式: (avg - frag) / (1.0 - avg)
    return max(avg_blocker_depth - ctx.fragmentDepth, 0.0) / (1.0 - avg_blocker_depth + 1e-6) * ctx.lightSizeWorld;
}

// PCSS 主函数
// 只需要传一个 ctx 进来
float calculate_pcss(ShadowContext ctx) {
    // Step 1: Blocker Search
    float search_radius_uv = ctx.lightSizeWorld / (ctx.shadowMapSize * ctx.fragmentDepth);
    
    float avg_blocker_depth = find_blocker(ctx, search_radius_uv);
    
    if (avg_blocker_depth < 0.0) return 1.0; // 无遮挡

    // Step 2: Penumbra Estimation (NDC Space)
    float penumbra_ndc = calculate_penumbra(ctx, avg_blocker_depth);
    
    // Convert to UV Space
    float penumbra_uv = penumbra_ndc / (ctx.shadowMapSize * ctx.clipW);
    penumbra_uv = clamp(penumbra_uv, 0.0, 0.1);

    // Step 3: Filtering (PCF)
    return calculate_pcf(ctx, penumbra_uv);
}

#endif