#ifndef MOER_RT_SHARED_HLSL
#define MOER_RT_SHARED_HLSL

// #include "framework/Common.hlsl"
#include "MathLib/STL.hlsli"

#pragma region [ rt geometry ]


#define INSTANCE_FLAG_DISABLE 0x1
#define INSTANCE_FLAG_DEFAULT 0x2
#define INSTANCE_FLAG_TRANSPARANT 0x4
#define INSTANCE_FLAG_EMISSION 0x8

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

#ifndef VULKAN
    // TODO: This code is not needed if HLSL 2021 is enabled. But currently
    // it works only for latest DXC from VK SDK. DXC from Win SDK crashes!
    // Keep an eye on "--hlsl2021" used in Cmake and C++.
    int3 select( bool3 cmp, int3 a, int3 b )
{
        int3 r;
        r.x = cmp.x ? a.x : b.x;
        r.y = cmp.y ? a.y : b.y;
        r.z = cmp.z ? a.z : b.z;

        return r;
    }

    float3 select( bool3 cmp, float3 a, float3 b )
    {
        float3 r;
        r.x = cmp.x ? a.x : b.x;
        r.y = cmp.y ? a.y : b.y;
        r.z = cmp.z ? a.z : b.z;

        return r;
    }
    #endif

float3 _GetXoffset( float3 X, float3 N )
{
    // TODO: try out: https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/

    // RT Gems "A Fast and Robust Method for Avoiding Self-Intersection" ( updated version taken from Falcor )
    // Moves the ray origin further from surface to prevent self-intersections, minimizes the distance.
    const float origin = 1.0 / 16.0;
    const float fScale = 3.0 / 65536.0;
    const float iScale = 3.0 * 256.0;

    // Per-component integer offset to bit representation of FP32 position
    int3 iOff = int3( N * iScale );

    // Select per-component between small fixed offset or variable offset depending on distance to origin
    float3 iPos = asfloat( asint( X ) + select( X < 0.0, -iOff, iOff ) );
    float3 fOff = N * fScale;

    return select( abs( X ) < origin, X + fOff, iPos );
}
struct RTPrimitive{
     uint3 indices;
     float world_uv_units;
};


#define RT_MATERIAL_INDEX_BIT_OFFSET 8
#define RT_MATERIAL_TYPE_MASK 0xff
struct RTHitInfo{
    float3 x;
    float3 x_prev;
    float3 v;
    float3 t;
    float3 n;
    float2 uv;
    float mip;
    float tmin;
    uint instance_id;
    uint material_type_and_id;
    uint flags;

    bool IsSky( )
    { return tmin == INF; }
    float3 GetXOffset(){
        return _GetXoffset(x, n);
    }
    bool IsTransparent(){
        return (flags & INSTANCE_FLAG_TRANSPARANT) != 0;
    }

    uint GetMaterialType(){
        return material_type_and_id & RT_MATERIAL_TYPE_MASK;
    }

    uint GetMaterialID(){
        return material_type_and_id >> RT_MATERIAL_INDEX_BIT_OFFSET;
    }
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
    uint material_type_and_id;
    uint flags;
    uint prim_offset;
    uint vtx_offset;

    uint GetMaterialType(){
        return material_type_and_id & RT_MATERIAL_TYPE_MASK;
    }
    uint GetMaterialID(){
        return material_type_and_id >> RT_MATERIAL_INDEX_BIT_OFFSET;
    }

};

struct RTMaterialProp{
    float3 l_direct; // unshadowed
    float3 l_emi;
    float3 n;
    float3 t;
    float3 base_color;
    float roughness;
    float metalness;
    float curvature;
};

namespace Raytracing{
    #pragma region [ material ]

    float2 GetConeAngleFromAngularRadius( float mip, float tan_con_angle )
    {
        // // In any case, we are limited by the output resolution
        tan_con_angle = max( tan_con_angle, rt_config.tan_pixel_angular_radius );

        return float2( mip, tan_con_angle );
    }

    float2 GetConeAngleFromRoughness( float mip, float roughness )
    {
        // return float2(mip, roughness);
        float con_angle = tan( ImportanceSampling::GetSpecularLobeHalfAngle( roughness ) ); // TODO:  * 0.33333?

        return GetConeAngleFromAngularRadius( mip, roughness );
    }

    float3 GetSamplingCoords( uint texture_handle, float2 uv, float mip, int mode )
    {
        float2 texSize;
        TextureHandle tex = TextureHandle( texture_handle );
        tex.GetTexture2D<float4>().GetDimensions( texSize.x, texSize.y );

        // Recalculate for the current texture
        float mip_num = log2( max( texSize.x, texSize.y ) );
        mip += mip_num - MAX_MIP;
        if( mode == MIP_VISIBILITY )
        {
            // We must avoid using lower mips because it can lead to significant increase in AHS invocations. Mips lower than 128x128 are skipped!
            mip = min( mip, mip_num - 7.0 );
        }
        else
            mip += rt_config.camera_origin_gmip_bias.w * ( mode == MIP_LESS_SHARP ? 0.5 : 1.0 );
        mip = clamp( mip, 0.0, mip_num - 1.0 );

        // #if( USE_LOAD == 1 )
        //     mip = round( mip );
        // #endif

        texSize *= exp2( -mip );

        // // Uv coordinates
        // #if( USE_LOAD == 1 )
        //     uv = frac( uv ) * texSize;
        // #endif

        return float3( uv, mip );
    }

    float3 GetLoadCoords( uint texture_handle, float2 uv, float mip, int mode )
    {
        float2 texSize;
        TextureHandle tex = TextureHandle( texture_handle );
        tex.GetTexture2D<float4>().GetDimensions( texSize.x, texSize.y );

        // Recalculate for the current texture
        float mip_num = log2( max( texSize.x, texSize.y ) );
        mip += mip_num - MAX_MIP;
        if( mode == MIP_VISIBILITY )
        {
            // We must avoid using lower mips because it can lead to significant increase in AHS invocations. Mips lower than 128x128 are skipped!
            mip = min( mip, mip_num - 7.0 );
        }
        else
            mip += rt_config.camera_origin_gmip_bias.w * ( mode == MIP_LESS_SHARP ? 0.5 : 1.0 );
        mip = clamp( mip, 0.0, mip_num - 1.0 );

        // #if( USE_LOAD == 1 )
            mip = round( mip );
        // #endif

        texSize *= exp2( -mip );

        // // Uv coordinates
        // #if( USE_LOAD == 1 )
            uv = frac( uv ) * texSize;
        // #endif

        return float3( uv, mip );
    }

    #pragma endregion
    float3 ReconstructViewPosition(float2 uv, float4 camera_frustum, float depth = 1.f, float orthomode = 0.0f){
        float3 p;
        p.xy = uv * camera_frustum.zw + camera_frustum.xy;
        p.xy *= depth * ( 1.f - abs(orthomode)) + orthomode;
        p.z = depth;
        return p;
    }


};
#endif