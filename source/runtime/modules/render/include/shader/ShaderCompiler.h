#ifndef MOERENGINE_SHADER_COMPILER_H
#define MOERENGINE_SHADER_COMPILER_H
#include "ShaderCommon.h"
#include "shader/ShaderCommon.h"
class ShaderCompiler {

public:
    static void ShaderConductorTest();

    /**
    * @brief cross-compile shader
    * 
    * @param input ShaderCompilerInput: contains all information compiler needs
    * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
    */
    static void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output);

private:
    static void CompileVulkan(const ShaderCompilerInput& input, ShaderCompilerOutput& output);
    static void CompileD3D12(const ShaderCompilerInput& input, ShaderCompilerOutput& output);

    static std::function<void(const ShaderCompilerInput& input, ShaderCompilerOutput& output)> g_compiler_func_table[EShaderPlatform::SP_Num];
};
#endif