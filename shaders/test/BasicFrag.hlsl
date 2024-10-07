struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};

RWStructuredBuffer<float> buffer : register(u0);

float4 main(PixelInput input) : SV_TARGET {
  buffer[0] = 0.5f;
  return float4(input.UV, 0.0f, 1.0f);
}