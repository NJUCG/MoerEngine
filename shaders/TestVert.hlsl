struct VSInput {
  float3 Position : POSITION;
  float3 Color : COLOR0;
};
Texture2D<float4> foo[5] : register(t2);

SamplerState samp[2] : register(s0);
SamplerState aniso : register(s2);

struct UBO {
  float4x4 projectionMatrix;
  float4x4 modelMatrix;
  float4x4 viewMatrix;
};

[[vk::push_constant]] ConstantBuffer<UBO> ubo : register(b0, space1);

struct VSOutput {
  float4 Position : SV_POSITION;
  float3 Color : COLOR0;
};

VSOutput main(VSInput input) {
  VSOutput output;
  output.Color = input.Color * 1.f;
  float4 vShadowDepths;
  float2 uv = float2(0, 0);
  [unroll] for (int i = 0; i < 1; i++) {
    vShadowDepths.x = foo[i].SampleLevel(samp[0], uv, 0).r;
  }
  //   vShadowDepths.x = foo[0].SampleLevel(samp[0], uv, 0).r;
  float4 pos = mul(ubo.projectionMatrix,
                   mul(ubo.viewMatrix,
                       mul(ubo.modelMatrix, float4(input.Position.xyz, 1.0))));
  output.Position = pos * vShadowDepths.x;
  return output;
}