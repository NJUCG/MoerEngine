#ifndef MOERENGINE_SHADER_COMPILER_H
#define MOERENGINE_SHADER_COMPILER_H

#include "ShaderCommon.h"
#include "misc/Types.h"
#include "shader/ShaderCommon.h"

struct ShaderCompiledFile {
    Moer::uint64 last_write_time;
    Moer::uint64 source_hash;
    Moer::uint64 compiled_hash;

    std::string path;
    std::string content;
};
using ShaderCompilerOutput = Moer::Render::ShaderCompilerOutput;
using ShaderCompilerInput  = Moer::Render::ShaderCompilerInput;
class IShaderCompiler {
public:
    virtual ShaderCompilerOutput* Compile(const ShaderCompilerInput& input) = 0;

    virtual ShaderCompilerOutput Compile(ShaderCompilerInput&& _input) = 0;
    /**
     * @brief Return If support target platform or shader type
     * 
     * @return true 
     * @return false 
     */
    virtual bool IsSupportTarget(const ShaderTargetInfo&) {
        return false;
    }
};

class RENDER_API ShaderCompiler {

public:
    static void Init();
    static void ShaderCompileTest();
    /**
    * @brief cross-compile shader
    * 
    * @param input ShaderCompilerInput: contains all information compiler needs
    * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
    */
    static ShaderCompilerOutput* Compile(const ShaderCompilerInput& input);

    static ShaderCompilerOutput Compile(ShaderCompilerInput&& _input);

private:
    static IShaderCompiler* compiler;
};
#endif