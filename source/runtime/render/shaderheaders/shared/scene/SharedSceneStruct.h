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

    // Cluster LOD 字段
    int cluster_group_id;    // 该 cluster 所属的 group ID（在 GClusterGroup[] 中的索引），-1 表示无 LOD
    int cluster_refined_id;  // 对应 clodCluster::refined —— 指向更精细的 group（-1 表示叶子 cluster）
};

/**
 * GClusterGroup: Cluster LOD Group 的 GPU 侧数据，用于运行时 LOD 选择
 *
 * 每个 group 存储其简化后的 bounding sphere 和误差值。
 * LOD 选择通过比较 screen-space error 与阈值来决定是否展开该 group 的子 cluster。
 */
struct GClusterGroup {
    float3 simplified_center;
    float  simplified_radius;
    float  simplified_error;
    int    depth;           // DAG 层级（0=leaf, 1+=简化层级），用于 debug 强制 LOD 选择
    int    parent_group_id; // 父 group（更粗层级），-1 表示根节点（最粗，无法被替代）
    uint   _pad1;
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
 *   一个 BLAS 对应一个 CMesh，BLAS 中只包含叶子 cluster（leaf clusters）。
 *   每个叶子 CPrimitive 在 BLAS 中对应一个 geometry（按 CMesh.primitive_entts
 *   的顺序，取前 num_leaf_clusters 个），因此 GeometryIndex() 返回值 =
 *   该叶子 CPrimitive 在 CMesh.primitive_entts 中的下标。
 *
 *   查询 primitive_id：
 *     primitive_id = rt_primitive_table[primitive_table_offset + GeometryIndex()]
 */
struct GRtInstance {
    float4x4 world_transform;         // 从 CNode 拷贝（避免依赖 raster 侧 m_instance_buf 排列）
    uint     primitive_table_offset;   // rt_primitive_table[] 的起始偏移
    uint     primitive_count;          // BLAS 中叶子 cluster 的数量（= GeometryIndex 有效范围）
    uint     first_primitive_id;       // 该 mesh 第一个叶子 CPrimitive 的 primitive_id
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