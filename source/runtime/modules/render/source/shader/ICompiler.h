#ifndef MOERENGINE_ICOMPILER_H
#define MOERENGINE_ICOMPILER_H

#include "shader/ShaderCommon.h"
class IShaderCompiler {

    virtual void CompileShader(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output, const std::string& _working_dir){

    };
};

#endif