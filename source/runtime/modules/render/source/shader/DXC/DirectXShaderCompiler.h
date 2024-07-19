#ifndef MOERENGINE_DXC_COMPILER_H
#define MOERENGINE_DXC_COMPILER_H

#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "../ShaderReflector.h"
class DXCompiler final : public IShaderCompiler {

public:
    ~DXCompiler();
    ShaderCompilerOutput* Compile(const ShaderCompilerInput& input) override;
    ShaderCompilerOutput  Compile(ShaderCompilerInput&& _input) override;

    static DXCompiler& GetInstance();
    bool               IsSupportTarget(const ShaderTargetInfo&) override;
    struct Impl;

private:
    DXCompiler();
    // static std::function<ShaderCompilerOutput*(const ShaderCompilerInput& input)> s_compiler_func_table[EShaderPlatform::SP_Num];

    // ShaderCompilerOutput* CompileD3D12(const ShaderCompilerInput& input);
    // ShaderCompilerOutput* CompileVulkan(const ShaderCompilerInput& input);

    // ShaderReflector* reflector;
    Impl* impl;
};
#endif