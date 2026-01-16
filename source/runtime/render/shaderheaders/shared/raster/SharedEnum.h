#pragma once

#ifdef __cplusplus
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
#else
#include "shared/raster/ShaderParametersUtils.h"
#endif

//Enum Definitions Begin
namespace Moer {
EnumParam(EShadingMode, DEFAULT_PBR, DEBUG);
EnumParam(EBrdfNdfMode, BECKMANN, GGX, GTR2, GTR1);
EnumParam(EBrdfGMode, G_SCHLICK, VIS_UE4, VIS_UNITY, VIS_FILAMENT, VIS_RESPAWN);
EnumParam(EAaMode, NONE, FXAA_SIMPLIFIED, FXAA_QUALITY, SMAA_1X, SMAA_T2X);
EnumParam(EAoMode, NONE, SSAO, SSAO_AO_ONLY, RTAO, RTAO_AO_ONLY, SSDO, SSDO_AO_ONLY, LINEARIZED_DEPTH_DIV_10);
EnumParam(EDenoiserMode, NONE, BILATERAL_FILTER);
EnumParam(ERtaoSampleMode, UNIFORM, COSINE_WEIGHTED);
EnumParam(EShadowMapMode, NONE, POINT_CUBE, CSM, CSM_AUTO);
// Used for glTF material alpha mode (gltf..Parser.cpp)
EnumParam(EAlphaMode, Opaque, Mask, Blend);
EnumParam(ELightType, None, Directional, Point, Spot, Ambient, Environment);
} // namespace Moer
