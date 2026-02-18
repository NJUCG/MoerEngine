#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] Texture2D<float4> bloom_tex : register(t0);
[[vk::binding(1, 0)]] SamplerState      linear_sampler : register(s0);
[[vk::push_constant]] ConstantBuffer<Moer::BloomApplyParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    float3 bloom_color = bloom_tex.Sample(linear_sampler, uv).rgb;
    return float4(bloom_color * param.bloom_intensity, 1.0);
}
