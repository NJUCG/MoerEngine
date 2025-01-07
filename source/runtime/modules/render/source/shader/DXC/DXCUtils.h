#ifndef MOER_ENGINE_DXC_UTILS_H
#define MOER_ENGINE_DXC_UTILS_H

#include "RenderAPI.h"
#include "include/spirv/unified1/spirv.h"
#include "shader/ShaderCommon.h"
#include "spirv_reflect.h"
EShaderParameterType ToShaderParameterType(SpvReflectResourceType _type, SpvReflectDescriptorType _desc_type);

ERHIPipelineStageFlags ToPipelineStageFlag(SpvReflectShaderStageFlagBits _stage);

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type);

EPixelFormat ToPixelFormat(SpvImageFormat _format);

std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform);

std::wstring SearchValidShaderPath(const std::string& _relative_shader_path);
#endif