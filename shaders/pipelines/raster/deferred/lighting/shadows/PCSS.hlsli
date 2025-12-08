#ifndef MOER_LIGHTING_SHADOWS_PCSS_HLSLI
#define MOER_LIGHTING_SHADOWS_PCSS_HLSLI

#include "pipelines/raster/deferred/lighting/shadows/PCF.hlsli"

// CONFIGURATION OF PCSS
// -----------------------------------------------------------------------

// Wheather some computation should be shared across 2x2 pixel quad.
//  0: disabled.
//  1: share occluder search result.
//  2: share occluder search and PCF results.
#define PCSS_SHARE_PER_PIXEL_QUAD 2

// Whether to debug pixel where they early return.
#define PCSS_DEBUG_EARLY_RETURN 0

// Wheather to set a maximum depth bias.
#define PCSS_MAX_DEPTH_BIAS 1

// Idea of the experiment to turn on.
#define PCSS_ANTI_ALIASING_METHOD 2

// Wheather to enable the sharpening filter after PCF for sharper edges than the shadow map resolution.
#define PCSS_ENABLE_POST_PCF_SHARPENING 1

// Blocker search samples
#define PCSS_SEARCH_BITS 4
#define PCSS_SEARCH_SAMPLES (1 << PCSS_SEARCH_BITS)

// Shadow filtering samples
#define PCSS_SAMPLE_BITS 4
#define PCSS_SAMPLES (1 << PCSS_SAMPLE_BITS)

float QuadAverage(float val) {
    float val0 = QuadReadLaneAt(val, 0);
    float val1 = QuadReadLaneAt(val, 1);
    float val2 = QuadReadLaneAt(val, 2);
    float val3 = QuadReadLaneAt(val, 3);
    return (val0 + val1 + val2 + val3) * 0.25;
}
float2 QuadAverage(float2 val) {
    return float2(QuadAverage(val.x), QuadAverage(val.y));
}

// 生成一个沿特定方向缩放的 2x2 矩阵
// Direction: 缩放的主轴方向 (必须归一化)
// ScaleMinusOne: (缩放倍数 - 1)
float2x2 GenerateDirectionalScale2x2Matrix(float2 Direction, float ScaleMinusOne)
{
    return float2x2(
       1.0 + ScaleMinusOne * Direction.x * Direction.x, ScaleMinusOne * Direction.y * Direction.x,
       ScaleMinusOne * Direction.x * Direction.y,       1.0 + ScaleMinusOne * Direction.y * Direction.y
    );
}


float find_blocker(ShadowContext ctx, float search_radius_uv,out float2 out_occluder_uv_center) {
    float num_blockers = 0;
    float blocker_depth_sum = 0.0;
    float2 blocker_uv_sum = float2(0, 0); 
    
    float2x2 rotation = GetRandomRotation(ctx.screenUV);
    float dynamicBias = GetSlopeScaledBias(ctx.normal, ctx.lightDir);

    #if PCSS_MAX_DEPTH_BIAS
        // 限制最大 Bias，防止漏光
        dynamicBias = min(dynamicBias, 0.0001);
    #endif

    [unroll]
    for (int i = 0; i < PCSS_SEARCH_SAMPLES; ++i) {
        float2 offset = mul(rotation, POISSON_DISK_16[i]) * search_radius_uv;
        float occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(ctx.shadowUV + offset).x;
        
        // Reverse-Z 逻辑
        //if (occluder_depth > ctx.fragmentDepth + SHADOW_BIAS) {
        if (occluder_depth > ctx.fragmentDepth + dynamicBias) {
            blocker_depth_sum += occluder_depth;
            blocker_uv_sum += offset;
            num_blockers+=1.0;  
        }
    }

    #if PCSS_SHARE_PER_PIXEL_QUAD >= 1
        // 计算当前像素的梯度，如果梯度太大（边缘），就减少共享权重，防止糊掉
        float2 deriv = max(abs(ddx(ctx.fragmentDepth)), abs(ddy(ctx.fragmentDepth)));
        float flat_factor = 1.0 - saturate(max(deriv.x, deriv.y) * 100.0); // 100.0 是敏感度参数
        
        // 只有在平坦区域才进行 Quad 共享
        if (flat_factor > 0.0) {
            float avg_num = QuadAverage(num_blockers);
            float avg_sum = QuadAverage(blocker_depth_sum);
            float2 avg_uv = QuadAverage(blocker_uv_sum);
            
            // 混合原始结果和 Quad 平均结果
            num_blockers = lerp(num_blockers, avg_num, flat_factor);
            blocker_depth_sum = lerp(blocker_depth_sum, avg_sum, flat_factor);
            blocker_uv_sum = lerp(blocker_uv_sum, avg_uv, flat_factor);
        }
    #endif
    
    if (num_blockers < 0.1) {
        out_occluder_uv_center = float2(0, 0);
        #if PCSS_DEBUG_EARLY_RETURN
            return -2.0; // 返回特殊值表示全亮 (Debug 红色/白色)
        #endif
        return -1.0; 
    }
    
    // 如果所有采样点都被遮挡
    if (num_blockers > (float(PCSS_SEARCH_SAMPLES) - 0.5)) {
         #if PCSS_DEBUG_EARLY_RETURN
            return -3.0; // 返回特殊值表示全黑
        #endif
    }
    
    float inv_num = 1.0 / num_blockers;
    out_occluder_uv_center = blocker_uv_sum * inv_num; // 重心
    return blocker_depth_sum * inv_num;
}

// 2. Penumbra Size
float calculate_penumbra(ShadowContext ctx, float avg_blocker_depth) {
    return max(avg_blocker_depth - ctx.fragmentDepth, 0.0) / (1.0 - avg_blocker_depth + 1e-6) * ctx.lightSizeWorld;
}

// PCSS 主函数
float calculate_pcss(ShadowContext ctx) {
    // Step 1: Blocker Search
    float search_radius_uv = ctx.lightSizeWorld / (ctx.shadowMapSize * ctx.fragmentDepth);
    float2 occluder_uv_gradient;// 接收遮挡物重心
    float avg_blocker_depth = find_blocker(ctx, search_radius_uv,occluder_uv_gradient);

    #if PCSS_DEBUG_EARLY_RETURN
        if (avg_blocker_depth == -2.0) return 0.8; // Debug: 浅灰表示 Early Out (全亮)
        if (avg_blocker_depth == -3.0) return 0.2; // Debug: 深灰表示 Early Out (全黑)
    #endif
    
    if (avg_blocker_depth < 0.0) return 1.0; // 无遮挡

    // Step 2: Penumbra Estimation (NDC Space)
    float penumbra_ndc = calculate_penumbra(ctx, avg_blocker_depth);
    
    float penumbra_uv = penumbra_ndc / (ctx.shadowMapSize * ctx.clipW);
    penumbra_uv = clamp(penumbra_uv, 0.0, 0.1);

    float shadow_visibility = calculate_pcf(ctx, penumbra_uv);

    #if PCSS_ENABLE_POST_PCF_SHARPENING
        const float AverageSampleDistance = sqrt(1.0 / (float(PCSS_SEARCH_SAMPLES) * PI));
        const float MaxSharpnessFactor = 4.0;
        
        // 一个纹素的大小
        float min_filter_size = 1.0 / ctx.shadowMapSize; 
        
        // 当前的半影半径
        float raw_filter_radius = penumbra_uv;

        // 4. 计算锐化衰减
        // 逻辑：如果遮挡物重心偏离中心很远 (length 大)，说明在边缘，需要锐化 (Fading -> 1)
        // 如果重心在中间 (length 小)，说明在内部，不需要锐化 (Fading -> 0)
        float2 normalized_gradient = occluder_uv_gradient / max(search_radius_uv, 1e-6);
        
        float sharpeness_fading = saturate(1.5 * (length(normalized_gradient) - AverageSampleDistance));

        // 5. 计算最终锐化强度
        // 如果半影 (raw_filter_radius) 很小，接近纹素大小 (min_filter_size)，说明是硬阴影，可以大力锐化
        // 如果半影很大，ratio 接近 0，lerp 结果接近 1 (不锐化)
        float radius_ratio = min_filter_size / max(raw_filter_radius, 1e-6);
        float final_sharpness_factor = lerp(1.0, clamp(radius_ratio, 1.0, MaxSharpnessFactor), sharpeness_fading);

        // 6. 应用锐化
        shadow_visibility = saturate(final_sharpness_factor * (shadow_visibility - 0.5) + 0.5);
    #endif

    return shadow_visibility;
}

#endif