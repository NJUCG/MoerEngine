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