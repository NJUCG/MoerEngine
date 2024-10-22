#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
struct VSInput {
  float3 Position : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float2 UV : TEXCOORD0;
};

struct VertexOutput {
  float4 Position : SV_POSITION;
  float2 UV : TEXCOORD0;
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
[[vk::push_constant]] ConstantBuffer<Constsant> param;

VertexOutput main(VSInput input, uint instance_id : SV_InstanceID) {

  ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);
  InstanceData instance_data =
      instance_data_array.Load<InstanceData>(instance_id);
  float4x4 model = instance_data.model2world;
  float4x4 modelInv = instance_data.inv_model2world;
  float4x4 mvp = mul(param.camera_view_proj, model);

  VertexOutput output;
  output.Position = mul(mvp, float4(input.Position, 1.0f));
  output.UV = input.UV;
  output.normal =
      normalize(mul(input.normal, float3x3(modelInv[0].xyz, modelInv[1].xyz,
                                           modelInv[2].xyz)));
  output.tangent = normalize(mul(model._11_12_13, input.tangent));
  output.InstanceID = instance_id;
  return output;
}