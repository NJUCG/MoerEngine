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
        uint v_buffer;
        uint g_buffer_normal;
        uint g_buffer_uv;
        uint g_buffer_depth;
        uint gbuffer_position;
        uint global_param_handle;
    };

    struct LightingData {
        Matrix4x4f inv_view_proj;
        uint       light_count;
        uint3      padding;
        float3     camera_position;
    };

    // MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
