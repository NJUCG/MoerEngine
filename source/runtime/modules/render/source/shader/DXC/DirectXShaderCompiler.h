#ifndef MOERENGINE_DXC_COMPILER_H
#define MOERENGINE_DXC_COMPILER_H

#include "shader/ShaderCompiler.h"
#include "../ShaderReflector.h"
class DXCompiler final : public IShaderCompiler {

public:
    ~DXCompiler();
    void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) override;

    static DXCompiler& GetInstance();
    bool               IsSupportTarget(const ShaderTargetInfo&) override;

private:
    DXCompiler();
    static std::function<void(const ShaderCompilerInput& input, ShaderCompilerOutput& output)> s_compiler_func_table[EShaderPlatform::SP_Num];

    void CompileD3D12(const ShaderCompilerInput& input, ShaderCompilerOutput& output);
    void CompileVulkan(const ShaderCompilerInput& input, ShaderCompilerOutput& output);

    ShaderReflector* reflector;
};
#endif