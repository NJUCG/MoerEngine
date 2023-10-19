#include "shader/ShaderResource.h"
#include "rhi/RHICommon.h"
#include <cstddef>
#include <mutex>
#include <shared_mutex>

const ShaderCodeEntry* ShaderCodeResourceMap::GetCodeEntry(const char* _key) {
    {
        std::shared_lock<std::shared_mutex> read_lock(rw_mutex);
        if (shader_code_entries.count(_key)) {
            return &shader_code_entries.find(_key)->second;
        }
    }
    return nullptr;
}

/**
 * @brief thread safe shader code output registration
 * 
 * @param _shader_sort_key shader file path
 * @param _output shader compiled result
 */
void ShaderCodeResourceMap::AddShaderCompilerOutput(std::string _shader_sort_key, const ShaderCompilerOutput& _output) {

    std::unique_lock<std::shared_mutex> write_lock(rw_mutex);
    shader_code_entries.emplace(_shader_sort_key, ShaderCodeEntry{_output.shader_code, _output.target_info.shader_type, _output.parameter_map});
}

/**
 * @brief Add constructed shader type to shader type resource map
 * 
 * @param type_name shader type map
 * @param shader constructed shader
 */
void ShaderTypeResourceMap::AddShader(const char* type_name, Shader* shader) {
    // assert()
    std::unique_lock<std::shared_mutex> read_lock(shared_mutex);
    shader_type_map.emplace(type_name, shader);
}

Shader* ShaderTypeResourceMap::FindOrAddShader(const char* type_name, Shader* shader) {

    {
        std::shared_lock<std::shared_mutex> read_lock(shared_mutex);
        if (shader_type_map.count(type_name)) {
            return shader_type_map.find(type_name)->second;
        }
    }
    if (shader == nullptr) return nullptr;
    //add shader
    std::unique_lock<std::shared_mutex> write_lock(shared_mutex);
    shader_type_map.emplace(type_name, shader);
    return shader;
}
ShaderTypeResourceMap::ShaderTypeResourceMap(EShaderPlatform _platform) {
}
ShaderTypeResourceMap::~ShaderTypeResourceMap() {
    for (auto& shader : shader_type_map) {
        shader.second->Delete();
    }
}