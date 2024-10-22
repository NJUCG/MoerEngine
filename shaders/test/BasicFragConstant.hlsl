#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
struct PixelInput {
  float4 Position : SV_POSITION;
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
};

[[vk::push_constant]] ConstantBuffer<Constsant> param;
//StructuredBuffer<InstanceData> instance_data : register(t0, space0);
Texture2D<float> texture : register(t0, space1);
SamplerState defaultSampler : register(s0, space0);

PS_OUTPUT main(PixelInput input) : SV_TARGET {
  float4 texColor = texture.Sample(defaultSampler, input.uv);
  TextureHandle tex = TextureHandle(param.texture);
  ArrayBuffer buf = ArrayBuffer(param.buffer);
  float4 value = buf.Load<float4>(0);
  float4 bdls_color = tex.Sample2D<float4>(input.uv);
  bdls_color.x = value.x;
  bdls_color.y = 0.f;
  bdls_color.z = 0.f;
  bdls_color = float4(input.uv, 0.f, 1.f);
	
  ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data);
  InstanceData instance_data = instance_data_array.Load<InstanceData>(input.InstanceID);
 
  PS_OUTPUT output;
  output.mat = instance_data.material_id << 8 |
               instance_data.material_type;
  output.normal = float4(input.normal * 0.5f + 0.5f, 1.0f);
  output.uv = input.uv;
  return output;
}