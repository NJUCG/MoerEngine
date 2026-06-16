#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "serialize/Serializer.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include <fstream>
#include <ostream>
#include <string_view>

namespace Moer::Render {
using std::move;

namespace {

std::string_view GetShaderTypeName(EShaderType shader_type) {
    switch (shader_type) {
        case ST_VERTEX: return "vertex";
        case ST_GEOMETRY: return "geometry";
        case ST_FRAGMENT: return "fragment";
        case ST_COMPUTE: return "compute";
        case ST_MESH: return "mesh";
        case ST_AMPLIFICATION: return "amplification";
        case ST_RAY_GEN: return "ray_gen";
        case ST_RAY_MISS: return "ray_miss";
        case ST_RAY_CLOSESTHIT: return "ray_closest_hit";
        case ST_RAY_CALLABLE: return "ray_callable";
        case ST_RAY_INTERSECTION: return "ray_intersection";
        case ST_RAY_ANYHIT: return "ray_any_hit";
        default: return "unknown";
    }
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

#pragma region[Shader Resource Cache]

bool ShaderResourcesCache::HasCache(const ShaderCompilerInput& _input) const {
    auto it = code_cache[_input.target_info.shader_type].shader_cache.find(_input);
    return it != code_cache[_input.target_info.shader_type].shader_cache.end();
}

void ShaderResourcesCache::RegisterCache(const ShaderCompilerInput& _input, ShaderCompilerOutput&& _output) {
    auto& cache = code_cache[_input.target_info.shader_type];
    auto  it    = cache.shader_cache.find(_input);
    if (it == cache.shader_cache.end()) {

        ShaderEntryKey key = cache.AllocateShaderKey(_input);

        cache.shader_entry_cache[key] = MakeShared<ShaderEntry>(
            (EShaderType)_input.target_info.shader_type,
            (EShaderPlatform)_input.target_info.shader_platform,
            std::move(_output.shader_code)
        );

        cache.shader_cache[_input] = MakeShared<Shader>(
            _output.parameter_map,
            _output.mutation_id,
            (EShaderType)_input.target_info.shader_type,
            StaticArray<uint64, 2>{_output.compiled_hash1, _output.compiled_hash2},
            uint64(_input.shader_name_hash),
            _input.entry_point,
            _input.relative_source_file_path,
            key,
            std::move(_output.source_dependencies)
        );
    }
}

#pragma endregion

struct ShaderManager::Impl {

    Impl(Render::RenderDevice& _device, ShaderManager& _manager) : device(_device), manager(_manager) {
        ShaderCompiler::Init();
    }
    Render::RenderDevice& device;
    ~Impl() = default;

    RasterPipelineConstructor Raster() {
        return RasterPipelineConstructor(device, manager);
    }

    RTConstructor Raytracing() {
        return RTConstructor(device);
    }

    ShaderManager& manager;
};

ShaderManager::ShaderManager(Render::RenderDevice& _device) {
    impl = MoerNew(Impl)(_device, *this);
    LoadCache(ConfigManager::GetInstance().GetEngineShaderCachedPath());
}

ShaderManager& ShaderManager::Get() {
    static ShaderManager manager(Render::RenderDevice::Get());
    return manager;
}

void ShaderManager::ShutDown() {
    if (Get().impl) {
        Get().DumpCache(ConfigManager::GetInstance().GetEngineShaderCachedPath());

        MoerDelete(Get().impl);
        Get().impl = nullptr;
    }
}

std::string GetCacheFileName(RenderDevice& _device) {
    return std::string(ToString(_device.GetShaderPlatform())) + ".sdc";
}
void ShaderManager::DumpCache(std::filesystem::path _path) {
    if (!std::filesystem::exists(_path)) {
        //make directories
        std::filesystem::create_directories(_path);
    }

    auto file_path = _path / GetCacheFileName(impl->device);

    std::ofstream fs(file_path, std::ios::binary);
    OutputStream  stream(fs);
    stream << shader_resources_cache;
}

void ShaderManager::LoadCache(std::filesystem::path _path) {
    if (!std::filesystem::exists(_path)) {
        //make directories
        std::filesystem::create_directories(_path);
    }

    auto file_path = _path / GetCacheFileName(impl->device);
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

Shader& ShaderManager::CompileShader(EShaderType _type, ShaderAsset&& _asset) {

    ShaderCompilerInput input{
        .target_info               = ShaderTargetInfo(_type, impl->device.GetShaderPlatform()),
        .mutation_id               = _asset.mutation_id,
        .entry_point               = _asset.entry_name,
        .relative_source_file_path = _asset.path,
        .shader_name               = _asset.path,
        .shader_name_hash          = GetHash(_asset.path),
        .environment               = std::move(_asset.environment)
    };

    auto it = shader_resources_cache.TryGetShader(input);

    // LOG_DEBUG("ShaderManager::CompileShader, FileName: {}, EntryName: {}, MutationID: {}, Environment: {} <=> {}",
    //           input.relative_source_file_path,
    //           input.entry_point,
    //           input.mutation_id,
    //           input.environment.ToString(),
    //           (it.first != nullptr ? "Cached" : "Compile"));

    static auto get_all_removed_target_strings =
        [](std::string input_string, const Array<std::string>& target_substring_array) -> std::string {
        for (const auto& target_substring : target_substring_array) {
            while (true) {
                size_t pos = input_string.find(target_substring);
                if (pos != std::string::npos) {
                    input_string =
                        input_string.substr(0, pos) + input_string.substr(pos + target_substring.length());
                } else {
                    break;
                }
            }
        }
        return input_string;
    };

    static auto replace_shared_directry_strings = [](std::string input_string) -> std::string {
        // shaderheaders
        // => source/runtime/render/shaderheaders
        size_t pos = input_string.find("shaderheaders");
        if (pos != std::string::npos) {
            input_string = input_string.substr(0, pos) + "source/runtime/render/shaderheaders" +
                           input_string.substr(pos + 13);
        }
        return input_string;
    };

    if (it.first != nullptr) {
        return *it.first;
    }

    const std::string     shader_path       = input.relative_source_file_path;
    const std::string     entry_point       = input.entry_point;
    const uint32_t        mutation_id       = input.mutation_id;
    const std::string_view shader_type_name = GetShaderTypeName(_type);

    auto&& output = ShaderCompiler::Compile(std::move(input));

    if (!output.b_succeeded) {
        LOG_WARNING(
            "Shader compile failed: type={}, path={}, entry={}, mutation_id={}",
            shader_type_name,
            shader_path,
            entry_point,
            mutation_id
        );
        for (const auto& error : output.errors) {
            std::string error_string = get_all_removed_target_strings(
                error,
                {"target/bin/Debug/asset/",
                 "target/bin/Release/asset/",
                 "target/Debug/bin/asset/",
                 "target/Release/bin/asset/"}
            );
            error_string = replace_shared_directry_strings(error_string);

            LOG_ERROR("Shader Compile Error: {}", error_string);
        }
        assert(
            false &&
            std::format("Shader Compile Error, FileName: {}", input.relative_source_file_path).c_str()
        );
        return *it.first;
    }
    shader_resources_cache.RegisterCache(input, std::move(output));
    return *shader_resources_cache.TryGetShader(input).first;
}

RenderDevice& ShaderManager::GetDevice() {
    return impl->device;
}

RasterPipelineConstructor ShaderManager::Raster() {
    return impl->Raster();
}
RasterPipelineConstructor::RasterPipelineConstructor(Render::RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    shader_manager(_manager) {}

RTConstructor ShaderManager::Raytracing() {
    return impl->Raytracing();
}

PipelineHandle RasterPipelineConstructor::CreatePipeline(
    GfxPsoCreateInfo&&        _pso_info,
    Array<std::string_view>&& _hash_values,
    Array<ShaderArgCppInfo>&& _arg_type_values
) {
    // get shaders and reflection
    auto is_empty = [](ShaderAssetOrCache& _asset) {
        if (!std::holds_alternative<ShaderAsset>(_asset)) {
            return false;
        }

        auto& asset = std::get<ShaderAsset>(_asset);
        return asset.path.empty() || asset.entry_name.empty();
    };
    bool b_vs_ps = is_empty(task_path) && !is_empty(vertex_path) && !is_empty(pixel_path);
    bool b_gs    = !is_empty(geometry_path);
    bool b_mesh  = !is_empty(mesh_path) && !is_empty(pixel_path);
    bool b_task  = !is_empty(task_path);

    auto target_info       = device.GetShaderPlatform();
    auto get_shader_output = [&](const ShaderAssetOrCache& _info, EShaderType _type) -> Shader& {
        if (std::holds_alternative<ShaderAsset>(_info) == false) {
            Shader& shader = *std::get<Shader*>(_info);

            return shader;
        }
        ShaderAsset asset = std::get<ShaderAsset>(_info);
        return shader_manager.CompileShader(_type, std::move(asset));
    };
    auto get_shader_info = [&](EShaderType _type, Shader& _output) {
        ShaderEntry& entry = shader_manager.GetShaderEntry(_output);
        return std::move(
            SingleShaderInfo{
                .name             = _output.shader_path,
                .entry_point      = _output.entry_name,
                .shader_data      = std::span<uint8_t>(entry.blob_data.data(), entry.blob_data.size()),
                .shader_type      = _type,
                .shader_param_map = &_output.reflection
            }
        );
    };
    PipelineShaderInfo sd_info{
        .layout_hash = std::move(_hash_values), .arg_cpp_info = std::move(_arg_type_values)
    };
    if (b_vs_ps) {
        auto& vert_output  = get_shader_output(vertex_path, ST_VERTEX);
        auto& pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);
        if (!b_gs) {
            sd_info.shader_group = ShaderVsPs{
                .vs = get_shader_info(ST_VERTEX, vert_output),
                .ps = get_shader_info(ST_FRAGMENT, pixel_output)
            };
        } else {
            auto& geo_output     = get_shader_output(geometry_path, ST_GEOMETRY);
            sd_info.shader_group = ShaderVsGsPs{
                .vs = get_shader_info(ST_VERTEX, vert_output),
                .gs = get_shader_info(ST_GEOMETRY, geo_output),
                .ps = get_shader_info(ST_FRAGMENT, pixel_output)
            };
        }
    }

    if (b_mesh) {
        auto& mesh_output  = get_shader_output(mesh_path, ST_MESH);
        auto& pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);

        if (!b_task) {
            sd_info.shader_group = ShaderMsPs{
                .ms = get_shader_info(ST_MESH, mesh_output), .ps = get_shader_info(ST_FRAGMENT, pixel_output)
            };
        } else {
            auto task_output     = get_shader_output(task_path, ST_AMPLIFICATION);
            sd_info.shader_group = ShaderTsMsPs{
                .ts = get_shader_info(ST_AMPLIFICATION, task_output),
                .ms = get_shader_info(ST_MESH, mesh_output),
                .ps = get_shader_info(ST_FRAGMENT, pixel_output)
            };
        }
    }

    return device.CreatePipeline(std::move(_pso_info), std::move(sd_info));
}

#pragma region[ compute pipeline ]

ComputeConstructor::ComputeConstructor(RenderDevice& _device, ShaderAsset&& _asset, ShaderManager& _mgr) :
    device(_device),
    shader_manager(_mgr),
    shader_info(std::move(_asset)) {}

PipelineShaderInfo ComputeConstructor::CompileShaderInfo(
    Array<std::string_view>&& _hash_values,
    Array<ShaderArgCppInfo>&& _arg_type_values
) {

    auto target_info       = device.GetShaderPlatform();
    auto get_shader_output = [&](const ShaderAssetOrCache& _info, EShaderType _type) -> Shader& {
        if (std::holds_alternative<ShaderAsset>(_info) == false) {
            Shader& shader = *std::get<Shader*>(_info);

            return shader;
        }

        ShaderAsset asset = std::get<ShaderAsset>(_info);
        return shader_manager.CompileShader(_type, std::move(asset));
    };
    auto get_shader_info = [&](EShaderType _type, Shader& _output) {
        ShaderEntry& entry = shader_manager.GetShaderEntry(_output);
        return std::move(
            SingleShaderInfo{
                .name             = _output.shader_path,
                .entry_point      = _output.entry_name,
                .shader_data      = std::span<uint8_t>(entry.blob_data.data(), entry.blob_data.size()),
                .shader_type      = _type,
                .shader_param_map = &_output.reflection
            }
        );
    };

    auto&              output = get_shader_output(shader_info, ST_COMPUTE);
    PipelineShaderInfo sd_info{
        .layout_hash = std::move(_hash_values), .arg_cpp_info = std::move(_arg_type_values)
    };
    sd_info.shader_group = ShaderCs{.cs = get_shader_info(ST_COMPUTE, output)};
    return std::move(sd_info);
}

PipelineHandle ComputeConstructor::CreatePipeline(
    Array<std::string_view>&& _hash_values,
    Array<ShaderArgCppInfo>&& _arg_type_values
) {
    return device.CreatePipeline(CompileShaderInfo(std::move(_hash_values), std::move(_arg_type_values)));
}

#pragma endregion

} // namespace Moer::Render
