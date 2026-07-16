

#include <core/math/STL.hlsli>
#include <shared/postprocess/ShaderParameters.h>
[[vk::binding(0, 0)]] ConstantBuffer<Moer::ToneMappingParams> params
    : register(b0);
[[vk::binding(1, 0)]] Texture2D source_tex : register(t0);
[[vk::binding(2, 0)]] Buffer<uint> exposure : register(t1);

[[vk::binding(3, 0)]] Texture2D color_lut : register(t2);
[[vk::binding(4, 0)]] SamplerState color_lut_sampler : register(s0);

float3 ConvertToLDR(float3 hdr_color) {
  float src_luminance = STL::Color::Luminance(hdr_color);
  if (src_luminance <= 0.0f) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  float adapted_luminance = asfloat(exposure[0]);

  if (adapted_luminance <= 0.0f) {
    return params.min_adapted_luminance;
  }
  float scaled_luminance =
      src_luminance * params.exposure_scale / adapted_luminance;
  float mapped_luminance =
      scaled_luminance *
      (1.0f + scaled_luminance * params.white_point_inv_squared) /
      (1.0f + scaled_luminance);

  return hdr_color * mapped_luminance / src_luminance;
}

float3 ApplyColorLUT(float3 color) {
  const float size = params.color_lut_size.y;

  color = saturate(color);
  float r = color.r * (size - 1.0f) + 0.5f;
  float g = color.g * (size - 1.0f) + 0.5f;
  float b = color.b * (size - 1.0f);

  float2 uv1 = float2((floor(b)) * size + r, g) * params.color_lut_size_inv;
  float2 uv2 =
      float2((ceil(b) + 1.0f) * size + r, g) * params.color_lut_size_inv;

  float3 c1 = color_lut.SampleLevel(color_lut_sampler, uv1, 0).rgb;
  float3 c2 = color_lut.SampleLevel(color_lut_sampler, uv2, 0).rgb;

  return lerp(c1, c2, frac(b));
}

float3 Dither(float3 input, float2 seed) {
  float rand = frac(sin(dot(seed, float2(12.9898, 78.233) * 2.0)) * 43758.5453);
  input = 255 * saturate(input);
  input = select(rand.xxx < (input - floor(input)), ceil(input), floor(input));
  input *= 1.0 / 255;
  return input;
}

void main(in float4 pos
          : SV_Position, in float2 uv
          : TEXCOORD0, out float4 target
          : SV_Target) {

  STL::Rng::Hash::Initialize(uint2(pos.xy), params.frame_idx);
  float2 seed = STL::Rng::Hash::GetFloat2();

  float4 hdr_color = source_tex[pos.xy];
  [branch] if (params.enabled) {
    target.rgb = Dither(ConvertToLDR(hdr_color.rgb), seed);
  }
  else {
    target.rgb = Dither(hdr_color.rgb, seed);
  }
  // target.rgb = hdr_color.rgb;
  target.a = hdr_color.a;

  if (params.color_lut_size.x > 0.0f) {
    target.rgb = ApplyColorLUT(target.rgb);
  }
}
