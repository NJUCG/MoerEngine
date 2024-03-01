struct PS_INPUT {
  float4 pos : SV_POSITION;
  float3 pos_w : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

[[vk::binding(0, 1)]] SamplerState defaultSampler;
[[vk::binding(1, 1)]] Texture2D baseColorMap;

float4 main(PS_INPUT input) : SV_Target {
  float4 out_col = baseColorMap.Sample(defaultSampler, input.uv);
  return out_col;
}