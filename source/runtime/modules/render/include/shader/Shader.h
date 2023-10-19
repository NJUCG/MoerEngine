#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "API_Macro.h"
#include "ShaderCommon.h"
#include "rhi/RHICommon.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderParameterMacros.h"
#include <stdint.h>
#include <vector>

struct ShaderParameterLayoutInfo{
    bool IsValid()const{
        return !(slot == -1 || space == -1 || type == EShaderParameterType::UNKNOWN);
    }
    ShaderParameterLayoutInfo(uint16_t _offset,
        uint16_t _stride,
        uint8_t _slot = -1,
        int8_t _space = -1,
        EShaderParameterType _type = EShaderParameterType::UNKNOWN)
        : offset(_offset),
        stride(_stride),
        slot(_slot),
        space(_space),
        type(_type){}
    uint16_t offset;
    uint16_t stride;

    int8_t slot;
    int8_t space;
    EShaderParameterType type;
};
struct ShaderRootParametersLayoutInfo{

    public:
    const std::vector<ShaderParameterLayoutInfo>& GetLayoutInfos()const{return layout_infos;}
    private:
    friend class Shader;
    std::vector<ShaderParameterLayoutInfo> layout_infos;

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
     * @brief Get the Code Entry object, contains compiled code and target platform
     * 
     * @return const ShaderCodeEntry* 
     */
    const class ShaderCodeEntry* GetCodeEntry() const;

    const ShaderRootParametersLayoutInfo& GetRootParametersLayoutInfo()const{return param_layout_info;}
protected:
    Hash64City compiled_hash;
private:
    void ConstructRootParameterLayoutInfo(const ShaderParametersInfoMap& _param_map);
private:
    const ShaderMetaType*   type;
    ShaderTargetInfo        target_info;
    
    ShaderRootParametersLayoutInfo param_layout_info;
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
