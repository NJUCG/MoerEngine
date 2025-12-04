#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::BilateralFilterDenoiserPipelineBindlessParam> param;

static const float3 LUMINANCE_WEIGHT = float3(0.299, 0.587, 0.114);

// 双边滤波
float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float3 color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;
    float  centerLuminance = dot(color, LUMINANCE_WEIGHT);

    float3 finalColor = float3(0.0f, 0.0f, 0.0f);
    float  totalWeight = 0.0f;

    int mn = -param.kernel_radius;
    int mx = param.kernel_radius;

    for (int y = mn; y <= mx; ++y) {
        for (int x = mn; x <= mx; ++x) {
            float2 uv1 = uv + float2(x, y) * param.inv_resolution;
            if (uv1.x < 0.0 || uv1.x > 1.0 || uv1.y < 0.0 || uv1.y > 1.0) continue;

            float3 sampleColor = TextureHandle(param.input_image).Sample2D<float3>(uv1);
            float  sampleLuminance = dot(sampleColor, LUMINANCE_WEIGHT);

            // 空间域权�?(高斯分布)
            float distSq = (float)(x * x + y * y);
            float spatialWeight = exp(-distSq / param.spatial_sigma_square);

            // 强度域权�?(高斯分布)
            // 使用亮度差异，你也可以使用颜色差异（例如 RGB 各自的差异平方和�?
            float colorDiffSq = (sampleLuminance - centerLuminance) * (sampleLuminance - centerLuminance);
            // float3 colorDiff = sampleColor - color;
            // float colorDiffSq = dot(colorDiff, colorDiff); // 如果想使用RGB颜色�?

            float rangeWeight = exp(-colorDiffSq / param.range_sigma_square);

            float weight = spatialWeight * rangeWeight;

            finalColor += sampleColor * weight;
            totalWeight += weight;
        }
    }

    // if (uv.x <= param.inv_resolution.x && uv.y <= param.inv_resolution.y) {
    //     printf(
    //         "finalColor: %f, %f, %f; totalWeight: %f; param.kernel_radius: %u\n",
    //         finalColor.x, finalColor.y, finalColor.z, totalWeight, param.kernel_radius
    //     );
    // }

    color = finalColor / totalWeight; // 正常输出
    // color = 0.5 * finalColor / totalWeight + 0.5 * color; // debug
    
    return float4(color, 1.0);
}