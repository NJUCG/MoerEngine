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
    float4x4 world2clip;
    uint     instance_data;
    uint     geometry_data;
    uint     geometry_instance_data;
};

struct ShadowDepthPassBindlessParam {
    float4x4 world2clip;
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
