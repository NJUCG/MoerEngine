#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] Texture2D<float4> input_tex : register(t0);
[[vk::binding(1, 0)]] SamplerState      linear_sampler : register(s0);
[[vk::push_constant]] ConstantBuffer<Moer::BloomPrefilterParam> param;

float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {

    float3 color      = input_tex.Sample(linear_sampler, uv).rgb;
    float  brightness = Luminance(color);

    // 软膝盖曲线计算
    float3 curve = float3(param.threshold - param.knee, param.knee * 2.0, 0.25 / (param.knee + 0.00001));
    float  rq    = clamp(brightness - curve.x, 0.0, curve.y);
    rq           = curve.z * rq * rq;

    // 最终增益因子
    float factor = max(rq, brightness - param.threshold) / max(brightness, 0.0001);

    // 3. 输出提取后的高亮颜色
    // 只有亮度超过 threshold 的部分才会有值
    float3 result = color * factor;

    // 防止出现负数（虽然 B10G11R11 是无符号的，但保持习惯）
    return float4(max(result, 0.0), 1.0);
}
