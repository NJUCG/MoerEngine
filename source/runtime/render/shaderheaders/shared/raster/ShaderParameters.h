/**
 * Include此文件即可
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
#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/SharedEnum.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
namespace Moer::Render {
#else
#define CONST const
#include "shaderheaders/shared/raster/SharedEnum.h"
#include "shared/raster/geometry_pass/ShaderParameters.h"
#include "shared/raster/lighting_pass/ShaderParameters.h"
#include "shared/raster/post_process/ShaderParameters.h"
namespace Moer {
#endif

// MARK: Main Content Begin

struct CopyPassBindlessParam {
    uint input_image;
    uint padding0;
    uint padding1;
    uint padding2;
};

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST