#ifndef MOER_LIGHTING_SHADOWS_PCSS_HLSLI
#define MOER_LIGHTING_SHADOWS_PCSS_HLSLI

// CONFIGURATION OF PCSS
// -----------------------------------------------------------------------

// Whether some computation should be shared across 2x2 pixel quad.
//  0: disabled.
//  1: share occluder search result.
//  2: share occluder search and PCF results.
#define PCSS_SHARE_PER_PIXEL_QUAD 2

// Whether to debug pixel where they early return.
#define PCSS_DEBUG_EARLY_RETURN 0

// Whether to set a maximum depth bias.
#define PCSS_MAX_DEPTH_BIAS 1

// Whether to use dynamic depth bias based on slope.
#define PCSS_DYNAMIC_DEPTH_BIAS 0

// Idea of the experiment to turn on.
#define PCSS_ANTI_ALIASING_METHOD 0

// Whether to enable the sharpening filter after PCF for sharper edges than the shadow map resolution.
#define PCSS_ENABLE_POST_PCF_SHARPENING 1

// Light Type
#define PCSS_LIGHT_TYPE 0 //0: Point/Spot Light, 1: Directional Light

// Blocker search samples
#define PCSS_SEARCH_BITS    5
#define PCSS_SEARCH_SAMPLES (1 << PCSS_SEARCH_BITS)

// Shadow filtering samples
#define PCSS_SAMPLE_BITS 5
#define PCSS_SAMPLES     (1 << PCSS_SAMPLE_BITS)

//必须先定义宏，再包含头文件
#include "pipelines/raster/deferred/lighting/shadows/PCF.hlsli"

BlockerStats find_blocker(ShadowContext ctx, float search_radius_uv) {
    BlockerStats stats;
    stats.blockerCase = 1;
    stats.avgDepth    = 0;
    stats.numBlockers = 0;
    stats.uvSum       = float2(0, 0);
    stats.uvSqSum     = float2(0, 0);
    stats.uvCrossSum  = 0;

    float2x2 rotation    = GetRandomRotation(ctx.screenUV);
    float    dynamicBias = GetSlopeScaledBias(ctx.normal, ctx.lightDir);

#if PCSS_DYNAMIC_DEPTH_BIAS
    dynamicBias = GetSlopeScaledBias(ctx.normal, ctx.lightDir);
#if PCSS_MAX_DEPTH_BIAS
    // 限制最大 Bias，防止漏光
    dynamicBias = min(dynamicBias, SHADOW_BIAS);
#endif
#else
    dynamicBias = SHADOW_BIAS;
#endif

    [unroll] for (int i = 0; i < PCSS_SEARCH_SAMPLES; ++i) {

        float2 raw_offset = mul(rotation, POISSON_DISK[i]);

        // 【抽象点 A】获取采样坐标 (float2 or float3)
        float3 sample_pos = GetShadowSamplingPos(ctx, raw_offset * search_radius_uv);

        // 【抽象点 B】采样深度
        float raw_occluder_depth = SampleShadowDepth(ctx, sample_pos);

        // 【抽象点 C】线性化 (为了正确的平均值计算和物理正确性)
        float linear_occluder_depth = LinearizeShadowDepth(ctx, raw_occluder_depth);

        if (is_shadowed(linear_occluder_depth, ctx.fragmentDepth, dynamicBias)) {
            stats.avgDepth += linear_occluder_depth;
            stats.numBlockers += 1.0;

            stats.uvSum += raw_offset;
            stats.uvSqSum += raw_offset * raw_offset;
            stats.uvCrossSum += raw_offset.x * raw_offset.y;
        }
    }

#if PCSS_SHARE_PER_PIXEL_QUAD >= 1
    // 计算当前像素的梯度，如果梯度太大（边缘），就减少共享权重，防止糊掉
    float2 deriv       = max(abs(ddx(ctx.fragmentDepth)), abs(ddy(ctx.fragmentDepth)));
    float  flat_factor = 1.0 - saturate(max(deriv.x, deriv.y) * 100.0); // 100.0 是敏感度参数

    // 只有在平坦区域才进行 Quad 共享
    if (flat_factor > 0.0) {
        stats.numBlockers = lerp(stats.numBlockers, QuadAverage(stats.numBlockers), flat_factor);
        stats.avgDepth    = lerp(stats.avgDepth, QuadAverage(stats.avgDepth), flat_factor);
        stats.uvSum       = lerp(stats.uvSum, QuadAverage(stats.uvSum), flat_factor);
        stats.uvSqSum     = lerp(stats.uvSqSum, QuadAverage(stats.uvSqSum), flat_factor);
        stats.uvCrossSum  = lerp(stats.uvCrossSum, QuadAverage(stats.uvCrossSum), flat_factor);
    }
#endif

    if (stats.numBlockers < 0.1) {
#if PCSS_DEBUG_EARLY_RETURN
        stats.blockerCase = 2;
        return stats; // 返回特殊值表示全亮 (Debug 红色/白色)
#endif
    }

    // 如果所有采样点都被遮挡
    if (stats.numBlockers > (float(PCSS_SEARCH_SAMPLES) - 0.5)) {
#if PCSS_DEBUG_EARLY_RETURN
        stats.blockerCase = 0;
        return stats; // 返回特殊值表示全黑
#endif
    }

    if (stats.numBlockers > 0.1) {
        stats.avgDepth /= stats.numBlockers;
    }

    return stats;
}

// PCSS
float calculate_pcss(ShadowContext ctx) {
    // Blocker Search
    float        search_radius_uv = get_search_radius_uv(ctx);
    BlockerStats stats            = find_blocker(ctx, search_radius_uv);

#if PCSS_DEBUG_EARLY_RETURN
    if (stats.blockerCase == 2)
        return 0.8; // Debug: 浅灰表示 Early Out (全亮)
    if (stats.blockerCase == 0)
        return 0.2; // Debug: 深灰表示 Early Out (全黑)
#endif

    if (stats.numBlockers < 0.1)
        return 1.0;

    float penumbra_uv = calculate_penumbra(ctx, stats.avgDepth);

#if PCSS_LIGHT_TYPE == 1
    // 给半影加一个最大值限制
    // 比如限制最大模糊半径为 32 个像素 (32.0 / 2048.0 ≈ 0.015)
    // 超过这个范围，阴影就不再变软了，防止采样崩坏
    float max_penumbra = 32.0 / ctx.shadowMapSize;
    penumbra_uv        = clamp(penumbra_uv, 0.0, max_penumbra);
#else
    penumbra_uv = clamp(penumbra_uv, 0.0, 0.1);
#endif

    // 计算自适应采样矩阵
    float2x2 pcf_matrix = float2x2(1, 0, 0, 1); // 默认为单位矩阵(圆形)

#if PCSS_ANTI_ALIASING_METHOD == 2
    {
        // 我们利用 SumWeight * Sum(XY) - Sum(X)*Sum(Y) 的形式来避免过早除法带来的精度损失
        float  N    = stats.numBlockers;
        float2 mean = stats.uvSum / N;

        // 协方差矩阵
        float  cov_xy = N * stats.uvCrossSum - stats.uvSum.x * stats.uvSum.y;
        float2 var    = N * stats.uvSqSum - stats.uvSum * stats.uvSum; // var.x, var.y

        // 特征值分解
        float trace    = var.x + var.y;
        float det_part = sqrt(max(0.0, trace * trace - 4.0 * (var.x * var.y - cov_xy * cov_xy)));

        float2 eigen_values = 0.5 * (trace + float2(det_part, -det_part));
        float  max_eigen    = max(eigen_values.x, eigen_values.y);
        float  min_eigen    = min(eigen_values.x, eigen_values.y);

        // 计算主特征向量
        float2 major_axis = normalize(float2(max_eigen - var.y, cov_xy));
        // 如果 cov_xy 接近0，上述计算可能不稳定，加个保护
        if (abs(cov_xy) < 1e-6)
            major_axis = float2(1, 0);

        // 计算椭圆拉伸系数
        float axis_ratio     = sqrt(max_eigen / max(min_eigen, 1e-6));
        float stretch_factor = axis_ratio - 1.3; // 1.3 是个经验阈值，只有比率超过它才开始拉伸

        // 混合因子：如果遮挡物太少(N<2)，协方差矩阵不可靠，就不要拉伸
        float covariance_fade = saturate((N >= 2.0 ? 1.0 : 0.0) * stretch_factor);

        // 启发式调整
        // 如果遮挡物重心偏离中心很远(mean很大)，说明是边缘，我们更倾向于相信这个方向
        // 这里简化了 UE 的复杂逻辑，直接取一个简单的混合
        float elliptical_factor = min(8.0 * stretch_factor, 6.0) * covariance_fade;

        // 生成变换矩阵
        pcf_matrix = GenerateDirectionalScale2x2Matrix(major_axis, elliptical_factor);
    }
#endif

    float shadow_visibility = calculate_pcf(ctx, penumbra_uv, pcf_matrix);

#if PCSS_ENABLE_POST_PCF_SHARPENING
    const float AverageSampleDistance = sqrt(1.0 / (float(PCSS_SEARCH_SAMPLES) * PI));
    const float MaxSharpnessFactor    = 4.0;

    // 一个纹素的大小
    float min_filter_size = 1.0 / ctx.shadowMapSize;

    // 当前的半影半径
    float raw_filter_radius = penumbra_uv;

    // 计算锐化衰减
    // 逻辑：如果遮挡物重心偏离中心很远 (length 大)，说明在边缘，需要锐化 (Fading -> 1)
    // 如果重心在中间 (length 小)，说明在内部，不需要锐化 (Fading -> 0)
    //float2 normalized_gradient = occluder_uv_gradient / max(search_radius_uv, 1e-6);
    float2 normalized_gradient = stats.uvSum / stats.numBlockers; // 使用遮挡物重心作为梯度近似

    float sharpeness_fading = saturate(1.5 * (length(normalized_gradient) - AverageSampleDistance));

    // 计算最终锐化强度
    // 如果radius_ratio很小，接近min_filter_size，说明是硬阴影，可以大力锐化（提高锐化上限到MaxSharpnessFactor）
    // 如果半影很大，ratio 接近 0，lerp 结果接近 1 (不锐化)
    float radius_ratio           = min_filter_size / max(raw_filter_radius, 1e-6);
    float final_sharpness_factor = lerp(1.0, clamp(radius_ratio, 1.0, MaxSharpnessFactor), sharpeness_fading);

    shadow_visibility = saturate(final_sharpness_factor * (shadow_visibility - 0.5) + 0.5);
#endif

    return shadow_visibility;
}

#endif