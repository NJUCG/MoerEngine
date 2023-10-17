#include "shader/ShaderResource.h"
#include <mutex>
#include <shared_mutex>

/**
 * @brief thread safe shader code output registration
 * 
 * @param _shader_sort_key shader file path
 * @param _output shader compiled result
 */
void ShaderCodeResourceMap::AddShaderCompilerOutput(std::string _shader_sort_key, const ShaderCompilerOutput& _output) {

    std::unique_lock<std::shared_mutex> write_lock(rw_mutex);
    shader_code_entries.emplace(_shader_sort_key, ShaderEntry{_output.shader_code, _output.target_info.shader_type, _output.parameter_map});
}
// int32_t ShaderCodeResourceMap::GetIndexByHash(const Hash64City& _hash) {
// auto pos = std::lower_bound(shader_hashes.begin(), shader_hashes.end(), _hash);
// if(pos == shader_hashes.end())return -1;
// return pos - shader_hashes.begin();
// }
