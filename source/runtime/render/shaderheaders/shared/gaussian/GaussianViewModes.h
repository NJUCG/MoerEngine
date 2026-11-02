#pragma once

#ifdef __cplusplus
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
#else
#include "shared/raster/ShaderParametersUtils.h"
#endif

// Preserve the enumerator order for existing scene files.
namespace Moer {
EnumParam(EGaussianDisplayMode, Reference, Engine);
EnumParam(EGaussianDebugView, Color, Transmittance, TileCoverage, ShadowVisibility, ShadowDepth);
} // namespace Moer
