#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "API_Macro.h"
#include "ShaderCommon.h"
#include "rhi/RHICommon.h"

typedef uint32_t ShaderResourceIndex;
class Shader {
    friend class ShaderMetaType;

public:
    RENDER_CORE_API Shader();

    RENDER_CORE_API Shader(const ShaderCompiledInitializer& intializer);

    ~Shader();
    virtual void Delete() {}
    //compiled shader hash
    RENDER_CORE_API const Hash64City& GetCompiledHash() const;

    uint32_t GetHashKey() const { return hash_key; }

    EShaderPlatform GetShaderPlatform() const { return static_cast<EShaderPlatform>(target_info.shader_platform); }
    EShaderType     GetShaderType() const { return static_cast<EShaderType>(target_info.shader_type); }
    // get resource index in resource map
    ShaderResourceIndex GetShaderResourceIndex() const { return resource_index; }

    static ShaderParametersMetadata* GetParametersMetaData() { return nullptr; }

protected:
    Hash64City compiled_hash;

private:
    const ShaderMetaType* type;
    ShaderTargetInfo      target_info;
    ShaderResourceIndex   resource_index;

    int32_t code_size;
    //compiled shader hash in 32 bit
    uint32_t hash_key;
};

#define DEFINE_SHADER_FUNCION_PROC(ShaderClassName) \
    static Shader* ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) { return new ShaderClassName(_initializer); }

#define ShaderFunctionProc(ShaderClassName) \
    ShaderClassName::ConstructShaderInstance

#define DEFINE_SHADER_TYPE(ShaderClassName, ShaderMapScope, API, ...) \
    INTERNAL_DEFINE_SHADER_TYPE(ShaderClassName, ShaderMapScope, API)

#define INTERNAL_DEFINE_SHADER_TYPE(ShaderClassName, ShaderMapScope, API) \
                                                                          \
public:                                                                   \
    using ShaderMapType = ShaderMapScope##ShaderMap;                      \
    static ShaderTypeRegistration s_registration;                         \
    static API ShaderMetaType&    GetMetaType();                          \
    DEFINE_SHADER_FUNCION_PROC(ShaderClassName)                           \
    ShaderClassName(const ShaderCompiledInitializer& _initializer) : Shader(_initializer) {}

#define IMPLEMENT_SHADER_TYPE(ShaderClassName, FileName, EntryPoint, ShaderType) \
    ShaderMetaType& ShaderClassName::GetMetaType() {                             \
        static ShaderMetaType s_meta_type(                                       \
            #ShaderClassName,                                                    \
            FileName,                                                            \
            EntryPoint,                                                          \
            ShaderType,                                                          \
            sizeof(ShaderClassName),                                             \
            ShaderClassName::GetParametersMetaData(),                            \
            ShaderFunctionProc(ShaderClassName));                                \
        return s_meta_type;                                                      \
    }                                                                            \
    ShaderTypeRegistration ShaderClassName::s_registration(ShaderClassName::GetMetaType);

#endif//MOERENGINE_SHADER_H
