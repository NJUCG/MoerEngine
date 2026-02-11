/**
 * 请统一Include如下文件，不要Include当前文件
 * CPP:
 *     #include "shaderheaders/shared/raster/ShaderParameters.h"
 * HLSL:
 *     #include "shared/raster/ShaderParameters.h"
 */
#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
//#define CONST constexpr
#include "misc/Traits.h"
namespace Moer::Render {
#else
//#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct GeometryPassBindlessParam {
    float4x4 world2clip;
    
    // Bindless handles for bindless rendering
    uint instance_buf_hdl;      // Array<GInstance>
    uint primitive_buf_hdl;    // Array<GPrimitive>
    uint position_buf_hdl;     // Array<float3>
    uint normal_buf_hdl;       // Array<uint> (packed normal)
    uint tangent_buf_hdl;       // Array<uint> (packed tangent)
    uint texcoord0_buf_hdl;    // Array<float2>
    
    // Legacy fields (deprecated, kept for compatibility)
    uint instance_data;
    uint geometry_data;
    uint geometry_instance_data;

    // about material & alpha test
    uint  material_buffer;
    uint  enable_alpha_test;
    float alpha_test_blend_pixel_cutoff;
};

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif