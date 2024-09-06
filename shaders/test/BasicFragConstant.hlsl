struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};
struct Constsant{
  float4 color;
};
[[vk::push_constant]] ConstantBuffer<Constsant> param;

float4 main(PixelInput input) : SV_TARGET {
    return float4(param.color.xyz, 1.0f);
}