struct PSInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> src_color : register(t0);
[[vk::binding(2, 0)]] SamplerState spl : register(s0);

float4 main(PSInput input) : SV_TARGET {
  return src_color.Sample(spl, input.uv);
}