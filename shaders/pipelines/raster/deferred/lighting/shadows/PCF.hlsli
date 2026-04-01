#ifndef MOER_LIGHTING_SHADOWS_PCF_HLSLI
#define MOER_LIGHTING_SHADOWS_PCF_HLSLI

#include "core/common/Bindless.hlsl"
#include "pipelines/raster/deferred/lighting/shadows/ShadowSampling.hlsli"

#define DEFINE_CALCULATE_PCF(SUFFIX)                                                                  \
    float CalculatePcf##SUFFIX(ShadowContext ctx, float penumbra_uv, float2x2 pcf_transform_matrix) { \
        float    visibility     = 0.0;                                                                \
        float2x2 rotation       = GetRandomRotation(ctx.screenUV);                                    \
        float    receiver_depth = GetFragmentDepth##SUFFIX(ctx);                                      \
                                                                                                      \
        float dynamicBias = SHADOW_BIAS;                                                              \
        if (PCSS_DYNAMIC_DEPTH_BIAS) {                                                                \
            dynamicBias = GetSlopeScaledBias(ctx.normal, ctx.lightDir);                               \
            if (PCSS_MAX_DEPTH_BIAS) {                                                                \
                dynamicBias = min(dynamicBias, SHADOW_BIAS);                                          \
            }                                                                                         \
        }                                                                                             \
                                                                                                      \
        [loop] for (int i = 0; i < PCSS_SAMPLES; ++i) {                                             \
            float2 disk_sample = mul(rotation, POISSON_DISK[i]);                                      \
            /* 应用椭圆变换 */                                                                        \
            float2 offset_uv             = mul(pcf_transform_matrix, disk_sample) * penumbra_uv;      \
            float3 sample_pos            = GetShadowSamplingPos##SUFFIX(ctx, offset_uv);              \
            float  raw_occluder_depth    = SampleShadowDepth##SUFFIX(ctx, sample_pos);                \
            float  linear_occluder_depth = LinearizeShadowDepth##SUFFIX(ctx, raw_occluder_depth);     \
            if (!IsShadowed##SUFFIX(linear_occluder_depth, receiver_depth, dynamicBias)) {            \
                visibility += 1.0;                                                                    \
            }                                                                                         \
        }                                                                                             \
        visibility /= float(PCSS_SAMPLES);                                                            \
                                                                                                      \
        if (PCSS_SHARE_PER_PIXEL_QUAD == 2) {                                                         \
            float2 deriv       = max(abs(ddx(ctx.fragmentDepth)), abs(ddy(ctx.fragmentDepth)));       \
            float  flat_factor = 1.0 - saturate(max(deriv.x, deriv.y) * 100.0);                       \
                                                                                                      \
            if (flat_factor > 0.0) {                                                                  \
                /* 混合当前像素的可见性和邻居的平均可见性 */                                          \
                /* 这里的 0.5 是混合强度，可以调成 1.0 让它完全平均，或者 0.5 保留一点自己的特征 */   \
                /* UE 原版代码这里用了 DoPerQuad * 0.5，也就是 flat_factor * 0.5 */                   \
                float avg_vis = QuadAverage(visibility);                                              \
                visibility    = lerp(visibility, avg_vis, flat_factor * 0.5);                         \
            }                                                                                         \
        }                                                                                             \
        return visibility;                                                                            \
    }

#endif