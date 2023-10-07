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

    RENDER_CORE_API Shader(const ShaderCompiledInfo& intializer);

    ~Shader();

    //shader source file hash
    RENDER_CORE_API const Hash64City& GetHash() const;
    RENDER_CORE_API const Hash64City& GetVertexHash() const;
    //compiled shader hash
    RENDER_CORE_API const Hash64City& GetOutputHash() const;

    uint32_t GetHashKey() const { return hash_key; }

    EShaderPlatform GetShaderPlatform() const { return static_cast<EShaderPlatform>(target_info.shader_platform); }
    EShaderType     GetShaderType() const { return static_cast<EShaderType>(target_info.shader_type); }
    // get resource index in resource map
    ShaderResourceIndex GetShaderResourceIndex() const { return resource_index; }

    static ShaderParametersMetadata* GetParametersMetaData() { return nullptr; }

protected:
    Hash64City compiled_hash;
    Hash64City source_hash;
    Hash64City vertex_hash;

private:
    ShaderMetaType*     type;
    ShaderTargetInfo    target_info;
    ShaderResourceIndex resource_index;

    int32_t num_samplers;
    int32_t code_size;
    //compiled shader hash in 32 bit
    uint32_t hash_key;
};

#define DEFINE_SHADER_TYPE(ShaderType, ShaderMapScope, API, ...) \
    INTERNAL_DEFINE_SHADER_TYPE(ShaderType, ShaderMapScope, API)

#define INTERNAL_DEFINE_SHADER_TYPE(ShaderClassName, ShaderMapScope, API) \
                                                                          \
public:                                                                   \
    using ShaderMapType = ShaderMapScope##ShaderMap;                      \
    static ShaderTypeRegistration s_registration;                         \
    static API ShaderMetaType&    GetStaticType();

#define IMPLEMENT_SHADER_TYPE(ShaderClassName, FileName, EntryPoint, ShaderType) \
    ShaderMetaType& ShaderClassName::GetStaticType() {                           \
        static ShaderMetaType s_meta_type(                                       \
            #ShaderClassName,                                                    \
            FileName,                                                            \
            EntryPoint,                                                          \
            ShaderType,                                                          \
            sizeof(ShaderClassName),                                             \
            ShaderClassName::GetParametersMetaData());                           \
        return s_meta_type;                                                      \
    }                                                                            \
    ShaderTypeRegistration ShaderClassName::s_registration(ShaderClassName::GetStaticType);

#endif//MOERENGINE_SHADER_H
