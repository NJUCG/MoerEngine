#include "framework/Common.hlsl"

[[vk::push_constant]] ConstantBuffer<CameraData> camera_data : register(b0);

StructuredBuffer<InstanceData> instance_data_buffer;

struct VS_INPUT {
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance_id : SV_InstanceID;
};

struct PS_INPUT {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input) {
  PS_INPUT output;
  float4x4 mvp = mul(camera_data.view_proj,
                     instance_data_buffer[input.instance_id].model2world);
  output.pos = mul(mvp, float4(input.pos, 1.f));
  output.uv = input.uv;
  output.col = float4(input.uv, 0.f, 1.f);
  return output;
}
