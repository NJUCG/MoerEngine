#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
namespace Moer::Render {
#else
#include "shared/raster/ShaderParametersUtils.h"
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

//MARK:Enum Definitions Begin
//gbuffer
//geometrypass

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST