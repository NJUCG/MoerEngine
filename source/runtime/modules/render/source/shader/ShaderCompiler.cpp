#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/vulkan/IVulkanRHI.h"
#include "shader/ShaderParameterMacros.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <execution>
#include <format>
#include <functional>
#include <stdint.h>
#include <utility>

#include "platform/Platform.h"

#include "shader/ShaderCompiler.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "log/LogSystem.h"
#include "shader/ShaderCommon.h"
#include "rhi/RHI.h"

#include "DXC/DirectXShaderCompiler.h"

#include "Core.h"

IShaderCompiler* ShaderCompiler::compiler = nullptr;
void             ShaderCompiler::Init() {
    //currently use dxc for all compile missions
    compiler = &(DXCompiler::GetInstance());
}

/**
 * @brief cross-compile shader
 * 
 * @param input ShaderCompilerInput: contains all information compiler needs
 * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
 */
ShaderCompilerOutput* ShaderCompiler::Compile(const ShaderCompilerInput& input) {
    //todo: need to consider include dependencies
    return compiler->Compile(input);
}

ShaderCompilerOutput ShaderCompiler::Compile(ShaderCompilerInput&& _input) {
    return compiler->Compile(std::move(_input));
}
