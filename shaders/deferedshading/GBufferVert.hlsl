#include "framework/Common.hlsl"

[[vk::push_constant]] ConstantBuffer<CameraData> camera_data : register(b0);
StructuredBuffer<InstanceData> instance_data : register(t0, space0);
struct VS_INPUT {
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  // float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance_id : INSTANCE_ID;
  uint iid : SV_INSTANCEID;
};

struct PS_INPUT {
  float4 pos : SV_POSITION;
  float3 pos_w : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  // float3 binormal : BINORMAL;
  float2 uv : TEXCOORD0;
  uint instance_id : INSTANCE_ID;
};

PS_INPUT main(VS_INPUT input) {
  PS_INPUT output;
  float4x4 model = instance_data[input.instance_id].model2world;
  float4x4 modelInv = instance_data[input.instance_id].inv_model2world;

  float3x3 model2world = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
  float3x3 world2model =
      float3x3(modelInv[0].xyz, modelInv[1].xyz, modelInv[2].xyz);

  output.normal = normalize(mul(input.normal, world2model));
  // output.binormal = normalize(mul(input.binormal, world2model));

  float4x4 mvp = mul(camera_data.view_proj, model);
  output.pos = mul(mvp, float4(input.pos, 1.f));
  output.pos_w = mul(model, float4(input.pos, 1.f)).xyz;
  output.uv = input.uv;
  output.tangent = mul(model2world, input.tangent);
  output.instance_id = input.iid;
  return output;
}
