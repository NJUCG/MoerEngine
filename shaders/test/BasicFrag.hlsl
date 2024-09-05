struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};

float4 main(PixelInput input) : SV_TARGET {
    return float4(input.UV, 0.0f, 1.0f);
}