#ifndef MOER_SHARED_RASTER_SHADER_PARAMETERS_H
#define MOER_SHARED_RASTER_SHADER_PARAMETERS_H

/**
 * Include此文件即可
 * CPP:
 *     #include "shaderheaders/shared/raster/ShaderParameters.h"
 * HLSL:
 *     #include "shared/raster/ShaderParameters.h"
 */

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
#include "shaderheaders/shared/raster/culling/ShaderParameters.h"
#include "shaderheaders/shared/raster/SharedEnum.h"
#include "shaderheaders/shared/raster/env_and_atmo_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include "shaderheaders/shared/raster/tessellated_surface/ShaderParameters.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"

namespace Moer::Render {
#else
#define CONST const
#include "shared/raster/ShaderParametersUtils.h"
#include "shared/raster/culling/ShaderParameters.h"
#include "shared/raster/SharedEnum.h"
#include "shared/raster/env_and_atmo_pass/ShaderParameters.h"
#include "shared/raster/geometry_pass/ShaderParameters.h"
#include "shared/raster/lighting_pass/ShaderParameters.h"
#include "shared/raster/post_process/ShaderParameters.h"
#include "shared/raster/tessellated_surface/ShaderParameters.h"
#include "shared/scene/SharedSceneStruct.h"

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

#endif // MOER_SHARED_RASTER_SHADER_PARAMETERS_H
