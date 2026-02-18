#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] Texture2D<float4> upsample_tex : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> downsample_tex : register(t1);
[[vk::binding(2, 0)]] SamplerState      linear_sampler : register(s0);
[[vk::push_constant]] ConstantBuffer<Moer::BloomUpsampleParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float x = param.inv_size.x * param.filter_radius;
    float y = param.inv_size.y * param.filter_radius;

    float3 upsampled;

    upsampled = upsample_tex.SampleLevel(linear_sampler, uv + float2(-x, -y), 0.0).rgb * 1.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(x, -y), 0.0).rgb * 1.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(-x, y), 0.0).rgb * 1.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(x, y), 0.0).rgb * 1.0;

    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(0, -y), 0.0).rgb * 2.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(0, y), 0.0).rgb * 2.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(-x, 0), 0.0).rgb * 2.0;
    upsampled += upsample_tex.SampleLevel(linear_sampler, uv + float2(x, 0), 0.0).rgb * 2.0;

    upsampled += upsample_tex.SampleLevel(linear_sampler, uv, 0.0).rgb * 4.0;

    upsampled *= 0.0625;

    float3 current = downsample_tex.SampleLevel(linear_sampler, uv, 0.0).rgb;

    float3 result = current + upsampled;

    return float4(result, 1.0);
}
