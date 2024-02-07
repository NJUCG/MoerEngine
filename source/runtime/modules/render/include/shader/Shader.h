#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "API_Macro.h"
#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderParameterMacros.h"

#include <cstdint>
/**
 * @brief Contains Layout info of a shader parameter,
    contains offset and stride in parameter structure
    in cpp end. And slot, space and Type in shader
 * 
 */
struct ShaderParameterLayoutInfo {

    /**
     * @brief means the defined parameter in cpp is not correctly reflected in corresponding shader
     * 
     * @return true 
     * @return false 
     */
    bool IsValid() const {
        return !(slot == -1 || space == -1 || type == EShaderParameterType::UNKNOWN);
    }
    ShaderParameterLayoutInfo(uint16_t             _offset,
                              uint16_t             _stride,
                              int8_t               _slot  = -1,
                              int8_t               _space = -1,
                              EShaderParameterType _type  = EShaderParameterType::UNKNOWN,
                              EShaderCodeResourceBindingType _resource_type = EShaderCodeResourceBindingType::INVALID)
        : offset(_offset),
          stride(_stride),
          slot(_slot),
          space(_space),
          type(_type),
          resource_type(_resource_type) {}
    uint16_t offset;
    uint16_t stride;

    int8_t               slot;
    int8_t               space;
    EShaderParameterType type;

    //only resources defined in shader root parameter struct have valid resource type, like srv, uav, cbv
    EShaderCodeResourceBindingType resource_type{EShaderCodeResourceBindingType::INVALID};
};

/**
 * @brief Contains Layout info of root shader parameters,
    contains all param info(except names) that was reflected
    in target shader.
 * 
 */
struct ShaderRootParametersLayoutInfo {

public:
    const Moer::Array<ShaderParameterLayoutInfo>& GetLayoutInfos() const { return layout_infos; }
    const Moer::Array<ShaderParameterLayoutInfo>& GetConstantsInfos() const { return constant_infos; }

private:
    friend class Shader;
    Moer::Array<ShaderParameterLayoutInfo> layout_infos;
    Moer::Array<ShaderParameterLayoutInfo> constant_infos;
};
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
    using TMutationSet        = TShaderMutationSetEmpty;
    using TMutationParameters = ShaderMutationParameters;
    RENDER_API Shader();

    RENDER_API Shader(const ShaderCompiledInitializer& intializer);

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
     * @brief Get the Shader Type
     * 
     * @return EShaderType 
     */
    EShaderType GetShaderType() const { return static_cast<EShaderType>(target_info.shader_type); }

    const ShaderMetaType* GetShaderMetaType() const { return type; }

    static ShaderParametersMetadata* GetParametersMetaData() { return nullptr; }

    const ShaderRootParametersLayoutInfo& GetRootParametersLayoutInfo() const { return param_layout_info; }

    static bool ShouldCompileMutation(const ShaderMutationParameters&) { return true; }

    static void SetCompileEnvironment(const ShaderMutationParameters&, ShaderCompilerEnvironment&) {}

protected:
    Hash64City compiled_hash;

protected:
    void ConstructRootParameterLayoutInfo(const ShaderParametersInfoMap& _param_map);

protected:
    const ShaderMetaType* type;
    ShaderTargetInfo      target_info;

    ShaderRootParametersLayoutInfo param_layout_info;

    int32_t code_size;
    //compiled shader hash in 32 bit
    uint32_t hash_key;
};

#define DEFINE_SHADER_FUNCION_PROC(ShaderClassName)                                                                                     \
    static Shader* ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) { return new ShaderClassName(_initializer); } \
    static void    SetCompileEnvironment(const ShaderMutationParameters& _mutation_params, ShaderCompilerEnvironment& _environment) {   \
        const typename ShaderClassName::TMutationSet set(_mutation_params.mutation_id);                                              \
        set.SetCompileEnvironment(_environment);                                                                                     \
    }
//vtable for ShaderMetaType
#define ShaderFunctionProc(ShaderClassName)     \
    ShaderClassName::ConstructShaderInstance,   \
        ShaderClassName::ShouldCompileMutation, \
        ShaderClassName::SetCompileEnvironment

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
            TMutationSet::mutation_count,                                        \
            ShaderFunctionProc(ShaderClassName));                                \
        return s_meta_type;                                                      \
    }                                                                            \
    ShaderTypeRegistration ShaderClassName::s_registration(ShaderClassName::GetMetaType);

class TestReflectionShader : public Shader {
    DEFINE_SHADER_TYPE(TestReflectionShader, Global, )
public:
    MUTATION_BOOL(TestBoolMutation);
    MUTATION_INT(TestUINT, 5, 0);
    DEFINE_MUTATION_SET(TestBoolMutation, TestUINT);

public:
    BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(Ubo)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, projectionMatrix)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, modelMatrix)
    DEFINE_SHADER_PARAM(Moer::Matrix4x4f, viewMatrix)

    END_SHADER_CONSTANT_STRUCT_DEFINITION(Ubo)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(Ubo, ubo)
    DEFINE_SHADER_PARAM_SRV(Buffer, bar)
    //Ubo set
    DEFINE_SHADER_PARAM_UAV(RWBuffer, dataLog)

    DEFINE_SHADER_PARAM_SAMPLER_ARRAY(Sampler[2], samp, 2)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, aniso)
    //srv set
    DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D[5], foo, 5)
    //uav se

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};
#endif//MOERENGINE_SHADER_H
