#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] Texture2D<float4> src_tex : register(t0);
[[vk::binding(1, 0)]] SamplerState      linear_sampler : register(s0);
[[vk::push_constant]] ConstantBuffer<Moer::BloomDownsampleParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float x = param.inv_size.x;
    float y = param.inv_size.y;

    float3 a = src_tex.SampleLevel(linear_sampler, uv + float2(-2 * x, 2 * y), 0.0).rgb;
    float3 b = src_tex.SampleLevel(linear_sampler, uv + float2(0, 2 * y), 0.0).rgb;
    float3 c = src_tex.SampleLevel(linear_sampler, uv + float2(2 * x, 2 * y), 0.0).rgb;

    float3 d = src_tex.SampleLevel(linear_sampler, uv + float2(-2 * x, 0), 0.0).rgb;
    float3 e = src_tex.SampleLevel(linear_sampler, uv + float2(0, 0), 0.0).rgb;
    float3 f = src_tex.SampleLevel(linear_sampler, uv + float2(2 * x, 0), 0.0).rgb;

    float3 g = src_tex.SampleLevel(linear_sampler, uv + float2(-2 * x, -2 * y), 0.0).rgb;
    float3 h = src_tex.SampleLevel(linear_sampler, uv + float2(0, -2 * y), 0.0).rgb;
    float3 i = src_tex.SampleLevel(linear_sampler, uv + float2(2 * x, -2 * y), 0.0).rgb;

    float3 j = src_tex.SampleLevel(linear_sampler, uv + float2(-x, y), 0.0).rgb;
    float3 k = src_tex.SampleLevel(linear_sampler, uv + float2(x, y), 0.0).rgb;
    float3 l = src_tex.SampleLevel(linear_sampler, uv + float2(-x, -y), 0.0).rgb;
    float3 m = src_tex.SampleLevel(linear_sampler, uv + float2(x, -y), 0.0).rgb;

    float3 result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.125;

    return float4(max(result, 0.0001), 1.0);
}
