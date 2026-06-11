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
    uint   type; // <=> ELightType, in shared/raster/SharedEnum.h
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
    // AABB for frustum culling (local space)
    float3 aabb_min;
    float  padding0; // padding to align to 16 bytes
    float3 aabb_max;
    uint   padding1; // padding to align to 16 bytes

    uint material_idx;
    uint attribute_mask;

    uint position_start_idx;       // in element (float3)
    uint packed_normal_start_idx;  // in element (uint)
    uint packed_tangent_start_idx; // in element (uint)
    uint texcoord0_start_idx;      // in element (float2)
    uint index_start_idx;          // in uint（index buffer的元素是以uint为单位，而非uint3）
};

/**
 * GInstance 和 CNode 是一一对应的
 * 
 * primitive_id: 反向映射到 Primitive/DrawIndex
 * - 因为目前HLSL标准不存在SV_DrawID这种用于定位DrawCall Index的变量
 * - 所以我们只能在Instance数据中对DrawCall Index进行反向索引
 * - 注：Draw Call Index == Primitive Index
 */
struct GInstance {
    float4x4 world_transform;
    uint     primitive_id; // Primitive ID (DrawIndex)，用于反向映射
};

/**
 * GRtInstance: RT 专用的 per-renderable instance 数据
 *
 * 与 GInstance 的区别：
 * - GInstance 是 raster 侧 per-CPrimitive 的 instance（与 draw call 1:1）
 * - GRtInstance 是 RT 侧 per-renderable 的 instance（与 TLAS instance 1:1）
 *
 * primitive_table_offset:
 *   指向 rt_primitive_table buffer 的起始偏移。
 *   shader 侧用 GeometryIndex() 加上此偏移查询 primitive_id。
 *
 *   GeometryIndex() 与 CPrimitive 的对应关系：
 *   一个 BLAS 对应一个 CMesh，该 CMesh 包含 N 个 CPrimitive。
 *   每个 CPrimitive 在 BLAS 中对应一个 geometry（按 CMesh.primitive_entts
 *   的顺序添加），因此 GeometryIndex() 返回值 = 该 CPrimitive 在
 *   CMesh.primitive_entts 中的下标。
 *
 *   查询 primitive_id：
 *     primitive_id = rt_primitive_table[primitive_table_offset + GeometryIndex()]
 */
struct GRtInstance {
    float4x4 world_transform;         // 从 CNode 拷贝（避免依赖 raster 侧 m_instance_buf 排列）
    uint     primitive_table_offset;   // rt_primitive_table[] 的起始偏移
    uint     primitive_count;          // 该 mesh 包含的 CPrimitive 数量
    uint     first_primitive_id;       // 该 mesh 第一个 CPrimitive 的 primitive_id
    uint     _padding_rt_instance;
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