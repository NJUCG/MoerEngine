#pragma once

#ifdef __cplusplus
#include "shaderheaders/shared/ShaderConstants.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#else
#include "shared/ShaderConstants.h"
#include "shared/raster/lighting_pass/ShaderParameters.h"
#endif

#define LIGHTING_PASS_CONSTANTS(X)              \
    X(MaterialPassBindlessParam, param)         \
    X(float, extra_ambient_intensity)
