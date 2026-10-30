#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "API_Macro.h"
#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderParameterMacros.h"

#include "misc/Types.h"
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
    ShaderParameterLayoutInfo(
        Moer::uint16                   _offset,
        Moer::uint16                   _stride,
        Moer::int8                     _slot          = -1,
        Moer::int8                     _space         = -1,
        Moer::int8                     _num           = -1,
        EShaderParameterType           _type          = EShaderParameterType::UNKNOWN,
        EShaderCodeResourceBindingType _resource_type = EShaderCodeResourceBindingType::INVALID
    ) :
        offset(_offset),
        stride(_stride),
        slot(_slot),
        space(_space),
        num(_num),
        type(_type),
        resource_type(_resource_type) {}
    Moer::uint16 offset;
    Moer::uint16 stride;

    Moer::int8           slot;
    Moer::int8           space;
    Moer::int8           num;
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
    const Moer::Array<ShaderParameterLayoutInfo>& GetBindingInfo() const {
        return binding_infos;
    }
    const Moer::Array<ShaderParameterLayoutInfo>& GetConstantsInfo() const {
        return constant_infos;
    }

private:
    friend class Shader;
    Moer::Array<ShaderParameterLayoutInfo> binding_infos;
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

#define DEFINE_SHADER_FUNCION_PROC(ShaderClassName)                                               \
    static Shader* ConstructShaderInstance(const ShaderCompiledInitializer& _initializer) {       \
        return new ShaderClassName(_initializer);                                                 \
    }                                                                                             \
    static void SetCompileEnvironment(                                                            \
        const ShaderMutationParameters& _mutation_params, ShaderCompilerEnvironment& _environment \
    ) {                                                                                           \
        const typename ShaderClassName::TMutationSet set(_mutation_params.mutation_id);           \
        set.SetCompileEnvironment(_environment);                                                  \
    }
//vtable for ShaderMetaType
#define ShaderFunctionProc(ShaderClassName)                                           \
    ShaderClassName::ConstructShaderInstance, ShaderClassName::ShouldCompileMutation, \
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
            ShaderFunctionProc(ShaderClassName)                                  \
        );                                                                       \
        return s_meta_type;                                                      \
    }                                                                            \
    ShaderTypeRegistration ShaderClassName::s_registration(ShaderClassName::GetMetaType);

#define IMPLEMENT_SHADER_TYPE_TEMPLATE(ShaderClassName, FileName, EntryPoint, ShaderType) \
    template<>                                                                            \
    ShaderMetaType& ShaderClassName::GetMetaType() {                                      \
        static ShaderMetaType s_meta_type(                                                \
            #ShaderClassName,                                                             \
            FileName,                                                                     \
            EntryPoint,                                                                   \
            ShaderType,                                                                   \
            sizeof(ShaderClassName),                                                      \
            ShaderClassName::GetParametersMetaData(),                                     \
            TMutationSet::mutation_count,                                                 \
            ShaderFunctionProc(ShaderClassName)                                           \
        );                                                                                \
        return s_meta_type;                                                               \
    }                                                                                     \
    template<>                                                                            \
    ShaderTypeRegistration ShaderClassName::s_registration(ShaderClassName::GetMetaType);
#endif //MOERENGINE_SHADER_H
