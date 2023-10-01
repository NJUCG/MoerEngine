#ifndef MOERENGINE_SHADER_H
#define MOERENGINE_SHADER_H

#include "ShaderProxy.h"
class ShaderPipelineType{
    enum class Type: uint8_t{
        Graphics,
        Mesh
    };
public:
    virtual ~ShaderPipelineType(){}
    const char* GetName() const { return name; }

    const char* name;
    Type type;
};

class ShaderGraphicsPipelineType : public ShaderPipelineType{
    ShaderType* vertex_shader;
    ShaderType* geometry_shader;
    ShaderType* fragment_shader;
    virtual ~ShaderGraphicsPipelineType(){

    }
};

class ShaderMeshPipelineType : public ShaderPipelineType{
    ShaderType* mesh_shader;
    ShaderType* amplification_shader;
    virtual ~ShaderMeshPipelineType(){

    }
};
struct ShaderCompilerOutput{

};


#endif//MOERENGINE_SHADER_H
