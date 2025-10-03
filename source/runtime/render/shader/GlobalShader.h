#ifndef MOER_GLOBAL_SHADER_H
#define MOER_GLOBAL_SHADER_H

#include "shader/Shader.h"

class GlobalShader : public Shader {
    friend class ShaderMetaType;

public:
    using TMutationSet        = TShaderMutationSetEmpty;
    using TMutationParameters = ShaderMutationParameters;
    RENDER_API GlobalShader();

    RENDER_API GlobalShader(const ShaderCompiledInitializer& intializer);

    ~GlobalShader();
    virtual void Delete() {}

    static ShaderParametersMetadata* GetParametersMetaData() { return nullptr; }


    static bool ShouldCompileMutation(const ShaderMutationParameters&) { return true; }

    static void SetCompileEnvironment(const ShaderMutationParameters&, ShaderCompilerEnvironment&) {}

protected:


private:


};
#endif