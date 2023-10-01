#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

//#include "ShaderProxy.h"
#include "RHIResource.h"
namespace Moer{

    struct ShaderParamBinding{
        uint32_t binding_slot;

    };


    class TShader{
    protected:
        std::string shader_name;
        EShaderType shader_type;
        EShaderPlatform target_platform;
        SHA256Hash hash;
        uint32_t shader_index;
    };
    struct ShaderDataRaw{
        std::vector<uint8_t> data;
        long long recent_compiled_time;
    };

    class GlobalShaderMap{
        std::unordered_map<std::string, TShader> map_data;
        std::vector<ShaderDataRaw> shader_meta_data;
    };
}

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
struct ShaderCompilerOutput{

};


#endif//MOERENGINE_SHADER_H
