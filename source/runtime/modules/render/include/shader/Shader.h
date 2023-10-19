#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "API_Macro.h"
#include "ShaderCommon.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderParameterMacros.h"

typedef uint32_t ShaderResourceIndex;
/**
 * @brief Shader Type information,
    contains:
    reflect parameter data from compiled info,
    targeted platform information
    meta type information
 * 
 */
class Shader {
    friend class ShaderMetaType;

public:
    RENDER_CORE_API Shader();

    RENDER_CORE_API Shader(const ShaderCompiledInitializer& intializer);

    ~Shader();
    virtual void Delete() {}

    /**
     * @brief Get the Compiled Hash object
     * 
     * @return const Hash64City& 
     */
    const Hash64City& GetCompiledHash() const;

    /**
    * @brief Get the Hash Key object
    * 
    * @return uint32_t 
    */
    uint32_t GetHashKey() const { return hash_key; }

    EShaderPlatform GetShaderPlatform() const { return static_cast<EShaderPlatform>(target_info.shader_platform); }

    /**
     * @brief Get the Shader Meta Type object
     * 
     * @return EShaderType 
     */
    EShaderType GetShaderMetaType() const { return static_cast<EShaderType>(target_info.shader_type); }

    static ShaderParametersMetadata* GetParametersMetaData() { return nullptr; }

    /**
     * @brief Get the Parameters Map object(generated from compiled data)
     * 
     * @return const ShaderParametersInfoMap& 
     */
    const ShaderParametersInfoMap& GetParametersMap() const { return param_map; }

    /**
     * @brief Get the Code Entry object, contains compiled code and target platform
     * 
     * @return const ShaderCodeEntry* 
     */
    const class ShaderCodeEntry* GetCodeEntry() const;

protected:
    Hash64City compiled_hash;

private:
    const ShaderMetaType*   type;
    ShaderTargetInfo        target_info;
    ShaderParametersInfoMap param_map;
    int32_t                 code_size;
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

class TestReflectionShader : public Shader {
    DEFINE_SHADER_TYPE(TestReflectionShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    //constant
    DEFINE_SHADER_PARAM(Moer::Vector4f, color)
    DEFINE_SHADER_PARAM_SRV(Buffer, bar)
    //Ubo set
    DEFINE_SHADER_PARAM_UAV(RWBuffer, dataLog)

    DEFINE_SHADER_PARAM_SAMPLER_ARRAY(Sampler[2], samp, 2)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, aniso)
    //srv set
    DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D[5], foo, 5)
    //uav set
    DEFINE_SHADER_PARAM_CBV(ConstantBuffer<UBO>, ubo)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
#endif//MOERENGINE_SHADER_H
