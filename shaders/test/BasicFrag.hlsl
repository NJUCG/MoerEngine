struct PixelInput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
};

RWStructuredBuffer<float> g_buffer : register(u0);

float4 main(PixelInput input) : SV_TARGET {
    g_buffer[0] = 0.5f;
    return float4(input.UV, 0.0f, 1.0f);
}