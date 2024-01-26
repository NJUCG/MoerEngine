#ifndef MOERENGINE_SHADER_COMPILER_H
#define MOERENGINE_SHADER_COMPILER_H
#include "ShaderCommon.h"
#include "shader/ShaderCommon.h"

struct ShaderCompiledFile {
    uint64_t last_write_time;
    uint64_t source_hash;
    uint64_t compiled_hash;

    std::string path;
    std::string content;
};
class IShaderCompiler {
public:
    virtual void Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) = 0;
    /**
     * @brief Return If support target platform or shader type
     * 
     * @return true 
     * @return false 
     */
    virtual bool IsSupportTarget(const ShaderTargetInfo&) { return false; }
};

class ShaderCompileJob {
public:
    void DispatchAndExecute(const std::function<void(const ShaderCompilerOutput&)>& post_process_func);
    void Finalize(const ShaderCompileJobInput& input);
    class Impl;

private:
    Impl* impl;
};
class ShaderCompiler {

public:
    static void Init();
    static void ShaderCompileTest();
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