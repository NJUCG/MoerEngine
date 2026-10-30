#include "misc/Types.h"
#include "shader/ShaderResourceManager.h"

#include "log/LogSystem.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace Moer::Render {

namespace {

constexpr uint32 k_shader_cache_magic          = 0x4D534443; // "MSDC"
constexpr uint32 k_shader_cache_format_version = 6;

std::string GetCacheFileName(RenderDevice& device) {
    return std::string(ToString(device.GetShaderPlatform())) + ".sdc";
}

uint64 GetNextShaderKeyOffset(const TypedShaderCache& cache) {
    uint64 next_key_offset = 0;

    for (const auto& [shader_input, shader_key] : cache.shader_cache_map) {
        next_key_offset = std::max(next_key_offset, shader_key.hash + 1);
    }
    for (const auto& [shader_key, shader_entry] : cache.shader_entry_cache) {
        next_key_offset = std::max(next_key_offset, shader_key.hash + 1);
    }

    return next_key_offset;
}

void RebuildLoadedShaderCacheRuntimeState(ShaderResourcesCache& cache) {
    for (auto& typed_cache : cache.code_cache) {
        typed_cache.key_offset = GetNextShaderKeyOffset(typed_cache);
    }
}

uint64 CountCachedShaders(const ShaderResourcesCache& cache) {
    uint64 cached_shader_count = 0;

    for (const auto& typed_cache : cache.code_cache) {
        cached_shader_count += typed_cache.shader_cache.size();
    }

    return cached_shader_count;
}

} // namespace

bool ShaderResourcesCache::HasCache(const ShaderCompilerInput& _input) const {
    auto it = code_cache[_input.target_info.shader_type].shader_cache.find(_input);
    return it != code_cache[_input.target_info.shader_type].shader_cache.end();
}

Shader& ShaderResourcesCache::RegisterCache(
    const ShaderCompilerInput& _input,
    ShaderCompilerOutput&&     _output
) {
    auto& cache = code_cache[_input.target_info.shader_type];
    if (const auto existing_shader = cache.shader_cache.find(_input);
        existing_shader != cache.shader_cache.end()) {
        return *existing_shader->second;
    }

    const ShaderEntryKey key = cache.AllocateShaderKey(_input);
    // Recompilation reuses the key and replaces its outdated bytecode.
    cache.shader_entry_cache.insert_or_assign(
        key,
        MakeShared<ShaderEntry>(
            (EShaderType)_input.target_info.shader_type,
            (EShaderPlatform)_input.target_info.shader_platform,
            std::move(_output.shader_code)
        )
    );

    const auto registered_shader = cache.shader_cache.try_emplace(
        _input,
        MakeShared<Shader>(
            _output.parameter_map,
            _output.mutation_id,
            (EShaderType)_input.target_info.shader_type,
            StaticArray<uint64, 2>{_output.compiled_hash1, _output.compiled_hash2},
            uint64(_input.shader_name_hash),
            _output.compiled_entry_name.empty() ? _input.entry_point : _output.compiled_entry_name,
            _input.relative_source_file_path,
            key,
            std::move(_output.source_dependencies)
        )
    ).first;
    return *registered_shader->second;
}

void ShaderManager::DumpCache(std::filesystem::path _path) {
    if (!std::filesystem::exists(_path)) {
        //make directories
        std::filesystem::create_directories(_path);
    }

    auto file_path = _path / GetCacheFileName(GetDevice());

    std::ofstream fs(file_path, std::ios::binary);
    OutputStream  stream(fs);
    stream << k_shader_cache_magic << k_shader_cache_format_version << shader_resources_cache;
    if (!fs.good()) {
        LOG_WARNING("[Startup][Shader] DumpCache failed to write shader cache: {}", file_path.string());
    }
}

void ShaderManager::LoadCache(std::filesystem::path _path) {
    if (!std::filesystem::exists(_path)) {
        //make directories
        std::filesystem::create_directories(_path);
    }

    auto file_path = _path / GetCacheFileName(GetDevice());
    if (!std::filesystem::exists(file_path)) {
        //create file
        std::ofstream fs(file_path, std::ios::binary);
        fs.close();
        LOG_DEBUG("[Startup][Shader] LoadCache created empty cache file: {}", file_path.string());
        return;
    }

    if (std::filesystem::file_size(file_path) == 0) {
        LOG_DEBUG("[Startup][Shader] LoadCache skipped empty cache file");
        return;
    }

    std::ifstream fs(file_path, std::ios::binary);
    if (!fs.is_open()) {
        LOG_WARNING(
            "[Startup][Shader] LoadCache failed to open cache file for reading. requested='{}', actual_open_path='{}'",
            file_path.string(),
            file_path.string()
        );
        return;
    }

    ShaderResourcesCache loaded_cache;
    InputStream          stream(fs);

    try {
        uint32 cache_magic   = 0;
        uint32 cache_version = 0;
        stream >> cache_magic >> cache_version;
        if (fs.fail()) {
            LOG_WARNING(
                "[Startup][Shader] LoadCache discarded truncated or unversioned shader cache: {}",
                file_path.string()
            );
            return;
        }
        if (cache_magic != k_shader_cache_magic || cache_version != k_shader_cache_format_version) {
            LOG_INFO(
                "[Startup][Shader] LoadCache discarded incompatible shader cache: path='{}', magic=0x{:08X}, version={}, expected_version={}",
                file_path.string(),
                cache_magic,
                cache_version,
                k_shader_cache_format_version
            );
            return;
        }
        stream >> loaded_cache;
    } catch (const std::exception& e) {
        LOG_WARNING(
            "[Startup][Shader] LoadCache caught exception during deserialization (cache format may have changed): {}",
            e.what()
        );
        return;
    } catch (...) {
        LOG_WARNING("[Startup][Shader] LoadCache caught unknown exception during deserialization, discarding cache");
        return;
    }

    if (fs.fail()) {
        LOG_WARNING("[Startup][Shader] LoadCache failed to deserialize shader cache: {}", file_path.string());
        return;
    }

    RebuildLoadedShaderCacheRuntimeState(loaded_cache);
    shader_resources_cache = std::move(loaded_cache);

    LOG_DEBUG(
        "[Startup][Shader] LoadCache loaded {} cached shaders",
        CountCachedShaders(shader_resources_cache)
    );
}

} // namespace Moer::Render
