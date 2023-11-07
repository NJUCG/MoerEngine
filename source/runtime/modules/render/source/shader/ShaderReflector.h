#ifndef MOER_SHADER_REFLECTOR_H
#define MOER_SHADER_REFLECTOR_H
#include <unordered_map>
#include <string>
#include "shader/ShaderCommon.h"

class ShaderReflector {
public:
    // Constructor
    ShaderReflector(){};

    // Destructor
    virtual ~ShaderReflector(){};

    // Member functions
    virtual void ReflectShader(const void* _compiled_result, const ShaderParametersMetadata* _param_meta_data, std::unordered_map<std::string, ParameterInfo>& _out_parameters) = 0;
};

#endif
