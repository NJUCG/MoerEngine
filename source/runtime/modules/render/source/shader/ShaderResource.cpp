#include "shader/ShaderResource.h"
#include "rhi/RHICommon.h"
#include <cstddef>
#include <mutex>
#include <shared_mutex>

// const ShaderCodeEntry* ShaderCodeResourceMap::GetCodeEntry(const char* _key) {
//     {
//         std::shared_lock<std::shared_mutex> read_lock(rw_mutex);
//         if (shader_code_entries.count(_key)) {
//             return &shader_code_entries.find(_key)->second;
//         }
//     }
//     return nullptr;
// }

// /**
//  * @brief thread safe shader code output registration
//  *
//  * @param _shader_sort_key shader file path
//  * @param _output shader compiled result
//  */
// void ShaderCodeResourceMap::AddShaderCompilerOutput(std::string _shader_sort_key, const ShaderCompilerOutput& _output) {

//     std::unique_lock<std::shared_mutex> write_lock(rw_mutex);
//     shader_code_entries.emplace(_shader_sort_key, ShaderCodeEntry{_output.shader_code, _output.target_info.shader_type, _output.parameter_map});
// }

ShaderResourceMap::ShaderResourceMap() {
}

ShaderResourceMap::~ShaderResourceMap() {
}

void ShaderResourceMap::AddShaderCompilerOutput(ShaderResourceKey key, const ShaderCompilerOutput& _output) {
    uint32_t code_index;
    {
        std::unique_lock<std::shared_mutex> write_lock(shader_code_entry_mutex);
        code_index = shader_code_entries.size();

        shader_code_entries[code_index] = ShaderCodeEntry{_output.shader_code, _output.target_info, _output.parameter_map};
    }
    //add shader resource map
    {
        std::unique_lock<std::shared_mutex> write_lock(map_mutex);
        if (shader_resource_map.contains(key)) {
            shader_resource_map[key].code_entry_index = code_index;
        } else {
            shader_resource_map.insert(std::make_pair(key, ShaderResourceMapContent{code_index, ~0u}));
        }
    }
}

/**
 * @brief Add constructed shader type to shader type resource map
 * 
 * @param type_name shader type map
 * @param shader constructed shader
 */
void ShaderTypeResourceMap::AddShader(ShaderTypeKey type_name, Shader* shader) {
    // assert()
    std::unique_lock<std::shared_mutex> read_lock(shared_mutex);
    shader_type_map.emplace(type_name, shader);
}

Shader* ShaderTypeResourceMap::FindOrAddShader(ShaderTypeKey type_name, Shader* shader) {

    {
        std::shared_lock<std::shared_mutex> read_lock(shared_mutex);

        auto count = shader_type_map.count(type_name);

        if (count) {
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

RHIShaderRef ShaderResourceMap::GetRHIShader(const ShaderResourceKey& key, const Shader* shader) {
    bool     has_key          = false;
    uint32_t code_index       = ~0u;
    uint32_t rhi_shader_index = ~0u;
    //check if shader resource map has key
    {
        std::shared_lock<std::shared_mutex> read_lock(map_mutex);
        has_key = shader_resource_map.contains(key);
        if (!has_key) {
            assert(false && std::format("shader resource map has no key for {}", shader->GetShaderMetaType()->GetName()).c_str());
            return nullptr;
        }
        ShaderResourceMapContent& content = shader_resource_map[key];

        code_index       = content.code_entry_index;
        rhi_shader_index = shader_resource_map[key].rhi_shader_index;

        assert(code_index != ~0u && "code index is invalid");
    }

    if (rhi_shader_index != ~0u) {
        std::shared_lock<std::shared_mutex> read_lock(rhi_shader_mutex);
        return rhi_shaders[rhi_shader_index];
    }
    //rhi shader creation
    RHIShaderRef result = nullptr;
    {
        ShaderCodeEntry* code_entry;
        {
            std::shared_lock<std::shared_mutex> read_lock(shader_code_entry_mutex);
            code_entry = &shader_code_entries[code_index];
        }
        switch (shader->GetShaderType()) {
            case ST_VERTEX:
                result = g_rhi->RHICreateVertexShader(code_entry, shader);
                break;
            case ST_GEOMETRY:
                result = g_rhi->RHICreateGeometryShader(code_entry, shader);
                break;
            case ST_FRAGMENT:
                result = g_rhi->RHICreateFragmentShader(code_entry, shader);
                break;
            case ST_COMPUTE:
                result = g_rhi->RHICreateComputeShader(code_entry, shader);
                break;
            case ST_MESH:
                result = g_rhi->RHICreateMeshShader(code_entry, shader);
                break;
            case ST_AMPLIFICATION:
                result = g_rhi->RHICreateAmplificationShader(code_entry, shader);
                break;
            case ST_RAY_GEN:
                result = g_rhi->RHICreateRayGenShader(code_entry, shader);
                break;
            case ST_RAY_MISS:
                result = g_rhi->RHICreateRayMissShader(code_entry, shader);
                break;
            case ST_RAY_CLOSESTHIT:
                result = g_rhi->RHICreateRayClosestHitShader(code_entry, shader);
                break;
            case ST_RAY_CALLABLE:
                result = g_rhi->RHICreateRayCallableShader(code_entry, shader);
                break;
            case ST_RAY_INTERSECTION:
                result = g_rhi->RHICreateRayIntersectionShader(code_entry, shader);
                break;
            case ST_RAY_ANYHIT:
                result = g_rhi->RHICreateRayAnyhitShader(code_entry, shader);
                break;
            default:
                throw std::runtime_error("not implemented");
        }
        assert(result != nullptr && "shader creation failed");
        {
            std::unique_lock<std::shared_mutex> write_lock(rhi_shader_mutex);
            rhi_shader_index = rhi_shaders.size();
            rhi_shaders.insert(std::make_pair(rhi_shader_index, result));
        }
        {
            //todo: may create multiple rhi shaders
            std::unique_lock<std::shared_mutex> write_lock(map_mutex);
            shader_resource_map[key].rhi_shader_index = rhi_shader_index;
        }
    }

    return result;
}