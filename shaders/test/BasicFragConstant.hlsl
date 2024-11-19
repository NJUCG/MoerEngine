#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
struct PixelInput {
  float4 Position : SV_POSITION;
  float3 world_position : POSITION;
  float2 uv : TEXCOORD0;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  int InstanceID : INSTANCEID;
};
struct Constsant {
  float4 color;
  uint texture;
  uint buffer;
  uint instance_data;
  float4x4 camera_view_proj;
};

struct PS_OUTPUT {
  uint mat : SV_TARGET0;
  float4 normal : SV_TARGET1;
  float2 uv : SV_TARGET2;
  float4 position : SV_TARGET3;
};

[[vk::push_constant]] ConstantBuffer<Constsant> param;
//StructuredBuffer<InstanceData> instance_data : register(t0, space0);
Texture2D<float> texture : register(t0, space1);
SamplerState defaultSampler : register(s0, space0);

PS_OUTPUT main(PixelInput input) : SV_TARGET {
  ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);
  InstanceData instance_data = instance_data_array.Load<InstanceData>(input.InstanceID);
 
  PS_OUTPUT output;
  output.mat = instance_data.material_id << 8 |
               instance_data.material_type;
  output.normal = float4(input.normal * 0.5f + 0.5f, 1.0f);
  output.uv = input.uv;
  output.position = float4(input.world_position, 1.0f);

  //printf("position: %f %f %f\n", input.world_position.x, input.world_position.y, input.world_position.z);
  return output;
}