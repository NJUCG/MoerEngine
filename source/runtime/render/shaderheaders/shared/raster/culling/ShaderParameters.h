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

// Defines the shared counter layout used by GPU culling readback and shaders.
struct GpuCullingCounterData {
    uint draw_count;
    uint visible_instance_count;
    uint total_instances_before;
    uint total_instances_after;
    uint visible_draws;
    uint total_draws;
    uint reserved0;
    uint reserved1;
};

#ifdef __cplusplus
}
#else
}
#endif