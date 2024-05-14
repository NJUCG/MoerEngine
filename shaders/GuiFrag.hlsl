struct PS_INPUT {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};
struct ProjectionMatrix {
  float4x4 mvp;
  bool need_correction;
};
[[vk::push_constant]] ConstantBuffer<ProjectionMatrix> vertexBuffer;
[[vk::binding(0, 0)]] SamplerState sampler0;
[[vk::binding(1, 0)]] Texture2D texture0;

template <typename Tscalar> Tscalar srgb_to_linear(Tscalar srgb) {
  // gamma correction
  // return srgb <= 0.04045 ? srgb / 12.92 : pow((srgb + 0.055) / 1.055, 2.4);
  return select(srgb / 12.92, pow((srgb + 0.055) / 1.055, 2.4), srgb > 0.04045);
}

float4 main(PS_INPUT input) : SV_Target {
  float4 out_col = input.col * texture0.Sample(sampler0, input.uv);
  float4 result = out_col;
  [branch] if (vertexBuffer.need_correction) {
    result = srgb_to_linear<float4>(out_col);
  }
  return result;
}