#include "shader/ShaderResource.h"
void ShaderCodeResourceMap::AddShaderCompilerOutput(const ShaderCompilerOutput& _output) {

    const Hash64City& compiled_hash = _output.compiled_hash;
    const auto& shader_code = _output.shader_code;

    auto pos = std::lower_bound(shader_hashes.begin(), shader_hashes.end(), compiled_hash);
    if(pos == shader_hashes.end()){
        shader_hashes.push_back(compiled_hash);
        shader_entries.emplace_back(shader_code, _output.target_info.shader_type);
    }
}
int32_t ShaderCodeResourceMap::GetIndexByHash(const Hash64City& _hash) {
    auto pos = std::lower_bound(shader_hashes.begin(), shader_hashes.end(), _hash);
    if(pos == shader_hashes.end())return -1;
    return pos - shader_hashes.begin();
}
