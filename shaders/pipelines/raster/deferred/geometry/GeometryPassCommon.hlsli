#pragma once

#if SHADOW_DEPTH_PASS

struct VsOutput {
    float4 position : SV_POSITION;
    float2 texcoord0 : TEXCOORD0;
    int material_id;
};

#else

struct VsOutput {
    float4 position : SV_POSITION;
    float3 world_position : POSITION;
    float2 texcoord0 : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    int material_id;
};

#endif