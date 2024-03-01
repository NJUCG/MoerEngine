#include "framework/Common.hlsl"

struct SceneUbo {
  float4x4 model;
  float4x4 view;
  float4x4 proj;
  float4x4 mvp;
};

[[vk::push_constant]] ConstantBuffer<CameraData> camera_data : register(b0);

struct VS_INPUT {
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance_id : INSTANCE_ID;
};

struct PS_INPUT {
  float4 pos : SV_POSITION;
  float3 pos_w : POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input) {
  PS_INPUT output;
  float4x4 model = 1.f;
  float4x4 mvp = mul(camera_data.proj, mul(camera_data.view, model));
  output.pos = mul(float4(input.pos, 1.f), mvp);
  output.pos_w = mul(float4(input.pos, 1.f), model).xyz;
  output.uv = input.uv;
  output.col = float4(input.uv, 0.f, 1.f);
  return output;
}
