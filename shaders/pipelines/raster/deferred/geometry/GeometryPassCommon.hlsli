#pragma once

#if SHADOW_DEPTH_PASS

struct VsOutput {
    float4 position : SV_POSITION;
    float2 texcoord0 : TEXCOORD0;
    nointerpolation uint material_id : MATERIAL_ID;
};

#else

struct VsOutput {
    float4 position : SV_POSITION;
    float3 world_position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 texcoord0 : TEXCOORD0;
    nointerpolation uint material_id : MATERIAL_ID;
};

#endif