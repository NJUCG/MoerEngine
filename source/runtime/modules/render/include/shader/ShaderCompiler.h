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
    void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output);
};
#endif