#ifndef MOER_ENGINE_DXC_UTILS_H
#define MOER_ENGINE_DXC_UTILS_H

// 提供 DXC 编译和反射过程使用的类型转换与路径辅助函数。
#include "PixelFormat.h"
#include "RenderAPI.h"
#include "shader/ShaderCommon.h"
#include "spirv.hpp"
#include "spirv_cross.hpp"

ERHIPipelineStageFlags ToPipelineStageFlag(spv::ExecutionModel _stage);

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type);

EPixelFormat ToPixelFormat(spv::ImageFormat _format);
std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform);

std::wstring SearchValidShaderPath(const std::string& _relative_shader_path);
#endif
