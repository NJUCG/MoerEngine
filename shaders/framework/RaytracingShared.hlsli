#pragma region [ rt geometry ]


#define INSTANCE_FLAG_DEFAULT 0x1
#define INSTANCE_FLAG_TRANSPARANT 0x2
#define INSTANCE_FLAG_EMISSION 0x4

#define INSTANCE_FLAG_GEOMETRY_ALL 0xff
#define INSTANCE_FLAG_GEOMETRY_NONE 0x00

struct RTVertex{
    float3 position;
    float uv0;
    float3 normal;
    float uv1;
    float3 tangent;
    float padding;
};

struct RTPrimitive{
     uint3 indices;
};


struct RTHitInfo{
    float3 x;
    float3 x_prev;
    float3 v;
    float3 t;
    float3 n;
    float2 uv;
    float tmin;
    uint instance_id;
};

#pragma endregion



struct RTViewParam{
    float4x4 view2world;
    float4x4 world2view;
    float4 frustum;
    float2 near_far;
    uint2 rect;
    float2 inv_rect;
    float2 jitter;
    float3 dir;
    float orthomode;
};

struct RTInstanceData{
    float4 overload_m1;
    float4 overload_m2;
    float4 overload_m3;
    uint material_id;
    uint material_type;
    uint prim_offset;
    uint vtx_offset;

};

namespace Raytracing{
    float3 ReconstructViewPosition(float2 uv, float4 camera_frustum, float depth = 1.f, float orthomode = 0.0f){
        float3 p;
        p.xy = uv * camera_frustum.zw + camera_frustum.xy;
        p.xy *= depth * ( 1.f - abs(orthomode)) + orthomode;
        p.z = depth;
        return p;
    }
};