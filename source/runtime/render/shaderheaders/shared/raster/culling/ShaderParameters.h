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
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

static const uint CULL_MAX_HIZ_MIPS              = 16u;
static const uint CULL_FLAG_ENABLE_HIZ_OCCLUSION = 1u << 0u;
static const uint CULL_FLAG_ENABLE_CLUSTER_LOD   = 1u << 1u;
static const uint CULL_FLAG_ENABLE_FRUSTUM       = 1u << 2u;

// Defines the shared counter layout used by GPU culling readback and shaders.
struct GpuCullingCounterData {
    uint draw_count;
    uint visible_instance_count;
    uint total_instances_before;
    uint total_instances_after;
    uint visible_draws;
    uint total_draws;
    uint frustum_culled_instances;
    uint occlusion_culled_instances;
    uint lod_culled_instances;
};

struct CullParams {
    uint draw_count;
    uint flags;
    uint hiz_mip_count;
    uint hiz_mip_handles[CULL_MAX_HIZ_MIPS];
};

struct CullData {
    float4   frustum_planes[6];
    float4x4 previous_view_proj;
    uint4    hiz_info;
    // Cluster LOD 参数
    float3   camera_position;
    float    camera_znear;
    float    camera_proj_11;     // projection[1][1] = 1 / tan(fovY/2)
    float    lod_error_threshold; // 屏幕空间误差阈值（归一化单位，UI 像素值 / viewport_height）
    int      force_lod_level;    // 强制 LOD 层级（-1 = auto, 0 = leaf, 1+ = 简化层级）
    uint     _pad_lod1;
};

struct HiZBuildParam {
    uint2 src_size;
    uint2 dst_size;
    uint  is_mip0;
};

#ifdef __cplusplus
}
#else
}
#endif