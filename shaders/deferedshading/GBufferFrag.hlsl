#include "framework/Common.hlsl"
#include "framework/Material.hlsl"

StructuredBuffer<InstanceData> instance_data : register(t0, space0);

struct PS_INPUT {
  float4 pos : SV_POSITION;
  float3 pos_w : POSITION;
  float3 normal : NORMAL;
  float3 tangent : TANGENT;
  float2 uv : TEXCOORD0;
  uint instance_id : INSTANCE_ID;
};


struct PS_OUTPUT {
    uint mat : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 position : SV_TARGET2;
};

PS_OUTPUT main(PS_INPUT input) : SV_Target { 
    PS_OUTPUT output;
    output.mat = instance_data[input.instance_id].material_id << 8 | instance_data[input.instance_id].material_type;
    // output.mat = 100;
    output.mat = input.instance_id;
    output.normal = float4(input.normal *0.5f +0.5f, 0.0f);
    output.position = float4(input.pos_w, 1.0f);
    return output;
 }