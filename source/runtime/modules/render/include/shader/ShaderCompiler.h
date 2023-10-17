#ifndef MOERENGINE_SHADER_COMPILER_H
#define MOERENGINE_SHADER_COMPILER_H
#include "ShaderCommon.h"
#include "shader/ShaderCommon.h"

class IShaderCompiler {
public:
    virtual void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) = 0;

    virtual bool IsSupportTarget(const ShaderTargetInfo&) { return false; }
};
class ShaderCompiler {

public:
    static void Init();
    static void ShaderConductorTest();

    /**
    * @brief cross-compile shader
    * 
    * @param input ShaderCompilerInput: contains all information compiler needs
    * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
    */
    static void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output);

private:
    static IShaderCompiler* compiler;
};
#endif