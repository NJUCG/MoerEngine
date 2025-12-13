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
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
namespace Moer::Render {
#else
#define CONST const
#include "shared/raster/ShaderParametersUtils.h"
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

//Enum Definitions Begin
namespace Moer {
EnumParam(EShadingMode, DEFAULT_PBR, DEBUG);
EnumParam(EBrdfNdfMode, BECKMANN, GGX, GTR2, GTR1);
EnumParam(EAaMode, NONE, FXAA_SIMPLIFIED, FXAA_QUALITY, SMAA_1X, SMAA_T2X);
EnumParam(EAoMode, NONE, SSAO, SSAO_AO_ONLY, RTAO, RTAO_AO_ONLY, SSDO, SSDO_AO_ONLY, LINEARIZED_DEPTH_DIV_10);
EnumParam(EDenoiserMode, NONE, BILATERAL_FILTER);
EnumParam(ERtaoSampleMode, UNIFORM, COSINE_WEIGHTED);
EnumParam(EShadowMapMode, NONE, POINT_CUBE, CSM, CSM_AUTO);
} // namespace Moer
