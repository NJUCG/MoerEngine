struct PS_INPUT {
  float4 pos : SV_POSITION;
  float3 pos_w : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float2 uv : TEXCOORD0;
};

float4 main(PS_INPUT input) : SV_Target { return float4(input.normal, 1.f); }