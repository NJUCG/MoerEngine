#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
// BINDLESS_BUFFER_TEXTURE(3, 2)
struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};
struct Constsant{
  float4 color;
  TextureHandle texture;
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;
Texture2D<float> texture : register(t0, space1);
SamplerState defaultSampler : register(s0, space0);

float4 main(PixelInput input) : SV_TARGET {
    float4 texColor = texture.Sample(defaultSampler, input.UV);
    float4 bdls_color = param.texture.Sample2D<float4>(input.UV);
    return float4(param.color * texColor * bdls_color);
}