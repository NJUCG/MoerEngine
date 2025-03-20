#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"

namespace Moer::Render {
#else
#define CONST const
namespace Moer {
#endif

    // MARK: Main Content Begin

    struct MaterialPassBindlessParam {
        uint material_type;
        uint light_buffer;
        uint material_buffer;
        uint vbuffer;
        uint gbuffer_normal;
        uint gbuffer_tangent;
        uint gbuffer_uv;
        uint gbuffer_depth;
        uint gbuffer_position;
        uint global_param_handle;
    };

    struct LightingData {
        float4x4 inv_view_proj;
        uint     light_count;
        uint3    padding;
        float3   camera_position;
    };

    // MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
