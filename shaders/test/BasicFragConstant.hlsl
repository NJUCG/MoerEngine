#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};
struct Constsant{
  float4 color;
  uint texture;
  uint buffer; 
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;
Texture2D<float> texture : register(t0, space1);
SamplerState defaultSampler : register(s0, space0);

float4 main(PixelInput input) : SV_TARGET {
    float4 texColor = texture.Sample(defaultSampler, input.UV);
    TextureHandle tex = TextureHandle(param.texture);

    float4 bdls_color = tex.Sample2D<float4>(input.UV);
    texColor.x = 1.0f;

    ArrayBuffer buf = ArrayBuffer(param.buffer);
    float v = buf.Load<float>(0);
    bdls_color.y = v;
    return float4(bdls_color);
}