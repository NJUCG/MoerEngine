#include "ShaderCommon.h"
#include "string_view"
void ShaderCompilerOutput::GenerateCompiledHash() {
    Hash64City& hash = compiled_hash;
    hash.Update(shader_code.data(), shader_code.size());

    auto param_map = parameter_map.GetShaderParameterMap();
    for(const auto& param : param_map){
        const auto& name = param.first;
        const auto& param_value = param.second;
        hash.Update(name.data(), name.length());
        hash.Update((const char*)(&param_value.type), sizeof(EShaderParameterType));
        hash.Update((const char*)(&param_value.buffer_index), sizeof(uint16_t));
        hash.Update((const char*)(&param_value.slot), sizeof(uint16_t));
        hash.Update((const char*)(&param_value.size), sizeof(uint16_t));
    }

}

