#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "ShaderCommon.h"
//namespace Moer{
//
//    struct ShaderParamBinding{
//        uint32_t binding_slot;
//
//    };
//
//
//    class TShader{
//    protected:
//        std::string shader_name;
//        EShaderType shader_type;
//        EShaderPlatform target_platform;
//        Hash64City      hash;
//        uint32_t shader_index;
//    };
//    struct ShaderDataRaw{
//        std::vector<uint8_t> data;
//        long long recent_compiled_time;
//    };
//
//    class GlobalShaderMap{
//        std::unordered_map<std::string, TShader> map_data;
//        std::vector<ShaderDataRaw> shader_meta_data;
//    };
//}


typedef uint32_t ShaderResourceIndex;
class Shader {
    friend class ShaderTypeInfo;

public:
    RENDER_CORE_API Shader();

    RENDER_CORE_API Shader(const ShaderCompiledInfo& intializer);

    ~Shader();

    //shader source file hash
    RENDER_CORE_API const Hash64City& GetHash()const;
    RENDER_CORE_API const Hash64City& GetVertexHash() const;
    //compiled shader hash
    RENDER_CORE_API const Hash64City& GetOutputHash() const;

    uint32_t GetHashKey() const{return hash_key;}

    EShaderPlatform GetShaderPlatform() const{return static_cast<EShaderPlatform>(target_info.shader_platform);}
    EShaderType GetShaderType() const{return static_cast<EShaderType>(target_info.shader_type);}
    // get resource index in resource map
    ShaderResourceIndex GetShaderResourceIndex() const{return resource_index;}

protected:
    Hash64City compiled_hash;
    Hash64City source_hash;
    Hash64City vertex_hash;

private:
    ShaderTypeInfo*      type;
    ShaderTargetInfo target_info;
    ShaderResourceIndex          resource_index;

    int32_t num_samplers;
    int32_t code_size;
    //compiled shader hash in 32 bit
    uint32_t hash_key;

};
//class ShaderPipelineType{
//    enum class Type: uint8_t{
//        Graphics,
//        Mesh
//    };
//public:
//    virtual ~ShaderPipelineType(){}
//    const char* GetName() const { return name; }
//
//    const char* name;
//    Type type;
//};
//
//class ShaderGraphicsPipelineType : public ShaderPipelineType{
//    ShaderType* vertex_shader;
//    ShaderType* geometry_shader;
//    ShaderType* fragment_shader;
//    virtual ~ShaderGraphicsPipelineType(){
//
//    }
//};
//
//class ShaderMeshPipelineType : public ShaderPipelineType{
//    ShaderType* mesh_shader;
//    ShaderType* amplification_shader;
//    virtual ~ShaderMeshPipelineType(){
//
//    }
//};


#endif//MOERENGINE_SHADER_H
