#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"
namespace Moer {
#else
#define CONST const
namespace Moer {
#endif

// MARK: Begin

/**
 * 此处结构体均以 G(Gpu) 开头
 */

/**
  * GLight 和 CLight 是一一对应的
  */
struct GLight {
    float3 color;
    float  intensity;
    float3 position;
    uint   type;
    float4 info;
    float3 direction;
};

#ifdef __cplusplus // C++ side

namespace GPrimitiveEAttributeMask {
CONST uint Position      = 1 << 0;
CONST uint PackedNormal  = 1 << 1;
CONST uint PackedTangent = 1 << 2;
CONST uint Texcoord0     = 1 << 3;
} // namespace GPrimitiveEAttributeMask

#else // HLSL side

namespace GPrimitiveEAttributeMask {
enum {
    Position      = 1 << 0,
    PackedNormal  = 1 << 1,
    PackedTangent = 1 << 2,
    Texcoord0     = 1 << 3,
};
} // namespace GPrimitiveEAttributeMask

#endif

/**
 * GPrimitive 和 CPrimitive 是一一对应的
 */
struct GPrimitive {
    uint material_idx;
    uint attribute_mask;

    uint position_offset;       // in bytes
    uint packed_normal_offset;  // in bytes
    uint packed_tangent_offset; // in bytes
    uint texcoord0_offset;      // in bytes
};

/**
 * GInstance 和 CNode&CTransform 是一一对应的
 */
struct GInstance {
    float4x4 world_transform;
};

/**
 * GMaterial 和 CMaterial 是一一对应的
 */
struct GMaterial {
    float4 albedo_factor;
    float3 emissive_factor;
    float  metallic_factor;
    float  roughness_factor;

    uint normal_map_hdl;
    uint ao_map_hdl;
    uint albedo_map_hdl;
    uint emissive_map_hdl;
    uint metallic_roughness_map_hdl;

    uint  alpha_mode;
    float alpha_cutoff;
};

// MARK: End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST