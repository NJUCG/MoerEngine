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

    struct GeometryPassBindlessParam {
        float4x4 camera_view_proj;
        float4   color;
        uint     texture;
        uint     buffer;
        uint     instance_data;
        uint     geometry_data;
        uint     geometry_instance_data;
    };

    // MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
