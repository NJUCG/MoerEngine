#ifndef MOER_ENGINE_DXC_UTILS_H
#define MOER_ENGINE_DXC_UTILS_H

#include "shader/ShaderCommon.h"
#include "spirv_reflect.h"
EShaderParameterType ToShaderParameterType(SpvReflectResourceType _type);

ERHIPipelineStageFlags ToPipelineStageFlag(SpvReflectShaderStageFlagBits _stage);

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type);

std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform);

#endif