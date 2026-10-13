#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/ShaderStageUtils.h"
#include "serialize/Serializer.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <string_view>

namespace Moer::Render {
using std::move;

namespace {

constexpr uint32_t k_shader_cache_magic          = 0x4D534443; // "MSDC"
constexpr uint32_t k_shader_cache_format_version = 4;

void ApplyShaderConstantLayoutDefines(
    ShaderAsset&                    asset,
    const PipelineArgumentMetadata& metadata,
    EShaderPlatform                 platform
) {
    if (platform == SP_WIN_D3D_SM6 && metadata.GetConstantLayouts().size() > 1) {
        throw std::invalid_argument("D3D12 does not yet support multiple logical constant arguments");
    }
    asset.environment.SetDefine("MOER_PC_LAYOUT_VERSION", uint32_t(1));
    asset.environment.SetDefine("MOER_PC_BYTE_SIZE", metadata.GetConstantByteSize());
    for (const ShaderConstantArgumentLayout& argument : metadata.GetConstantLayouts()) {
        const std::string_view name = metadata.GetArguments()[argument.argument_index].name;
        asset.environment.SetDefine("MOER_PC_OFFSET_" + std::string(name), argument.byte_offset);
        asset.environment.SetDefine("MOER_PC_SIZE_" + std::string(name), argument.byte_size);
    }
}

struct ShaderStageSource {
    EShaderType         shader_type;
    const ShaderSource* source;
};

bool IsShaderSourceEmpty(const ShaderSource& source) {
    if (const auto* shader = std::get_if<Shader*>(&source)) {
        return *shader == nullptr;
    }
    const ShaderAsset& asset = std::get<ShaderAsset>(source);
    return asset.path.empty() || asset.entry_name.empty();
}

Array<ShaderStageSource> CollectGraphicsShaderSources(std::span<const ShaderStageSource> candidates) {
    Array<ShaderStageSource> stages;
    stages.reserve(candidates.size());
    for (const ShaderStageSource& stage : candidates) {
        if (!IsShaderSourceEmpty(*stage.source)) {
            stages.push_back(stage);
        }
    }
    return stages;
}

std::string_view ValidateGraphicsShaderSources(std::span<const ShaderStageSource> stages) {
    // Only stage types are needed to reject invalid combinations before compilation.
    Array<SingleShaderInfo> stage_infos;
    stage_infos.reserve(stages.size());
    for (const ShaderStageSource& stage : stages) {
        stage_infos.push_back(SingleShaderInfo{.shader_type = stage.shader_type});
    }
    return ValidateGraphicsShaderStages(stage_infos);
}

void ApplyGraphicsShaderDefines(
    ShaderSource&   source,
    EShaderType     shader_type,
    EShaderPlatform platform,
    bool            uses_tessellation
) {
    if (!uses_tessellation || platform != SP_METAL_MSL || shader_type != ST_VERTEX) {
        return;
    }
    if (ShaderAsset* asset = std::get_if<ShaderAsset>(&source)) {
        asset->environment.SetDefine("MOER_METAL_TESS_VS_CAPTURE", true);
        asset->environment.SetDefine("MOER_METAL_TESS_VS_GRID_BUFFER", true);
    }
}

SingleShaderInfo BuildSingleShaderInfo(ShaderManager& manager, Shader& shader) {
    ShaderEntry& entry = manager.GetShaderEntry(shader);
    return SingleShaderInfo{
        .name               = shader.shader_path,
        .entry_point        = shader.entry_name,
        .shader_data        = std::span<uint8_t>(entry.blob_data.data(), entry.blob_data.size()),
        .shader_type        = shader.type,
        .shader_param_map   = &shader.reflection,
        .compute_local_size = shader.reflection.compute_local_size
    };
}

std::string FormatShaderCompileDiagnostic(std::string diagnostic) {
    constexpr std::string_view asset_prefixes[] = {
        "target/bin/Debug/asset/",
        "target/bin/Release/asset/",
        "target/Debug/bin/asset/",
        "target/Release/bin/asset/"
    };
    for (std::string_view prefix : asset_prefixes) {
        size_t position = 0;
        while ((position = diagnostic.find(prefix)) != std::string::npos) {
            diagnostic.erase(position, prefix.size());
        }
    }
    constexpr std::string_view shared_directory = "shaderheaders";
    if (const size_t position = diagnostic.find(shared_directory); position != std::string::npos) {
        diagnostic.replace(position, shared_directory.size(), "source/runtime/render/shaderheaders");
    }
    return diagnostic;
}

std::string_view GetShaderTypeName(EShaderType shader_type) {
    switch (shader_type) {
        case ST_VERTEX: return "vertex";
        case ST_GEOMETRY: return "geometry";
        case ST_FRAGMENT: return "fragment";
        case ST_COMPUTE: return "compute";
        case ST_MESH: return "mesh";
        case ST_AMPLIFICATION: return "amplification";
        case ST_HULL: return "hull";
        case ST_DOMAIN: return "domain";
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
        uint32_t cache_magic   = 0;
        uint32_t cache_version = 0;
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

    if (Shader* cached_shader = shader_resources_cache.TryGetShader(input).first) {
        return *cached_shader;
    }

    const std::string      shader_path       = input.relative_source_file_path;
    const std::string      entry_point       = input.entry_point;
    const uint32_t         mutation_id       = input.mutation_id;
    const std::string_view shader_type_name = GetShaderTypeName(_type);

    ShaderCompilerOutput output = ShaderCompiler::Compile(std::move(input));

    if (!output.b_succeeded) {
        LOG_WARNING(
            "Shader compile failed: type={}, path={}, entry={}, mutation_id={}",
            shader_type_name,
            shader_path,
            entry_point,
            mutation_id
        );
        for (const auto& error : output.errors) {
            LOG_ERROR("Shader Compile Error: {}", FormatShaderCompileDiagnostic(error));
        }
        throw std::runtime_error(std::format(
            "Shader compile failed: type={}, path={}, entry={}, mutation_id={}",
            shader_type_name,
            shader_path,
            entry_point,
            mutation_id
        ));
    }
    return shader_resources_cache.RegisterCache(input, std::move(output));
}

Shader& ShaderManager::ResolvePipelineShader(
    ShaderSource                    source,
    EShaderType                     shader_type,
    const PipelineArgumentMetadata& argument_metadata
) {
    if (const auto* shader = std::get_if<Shader*>(&source)) {
        if (*shader == nullptr || (*shader)->type != shader_type) {
            throw std::invalid_argument("Pipeline shader source is null or has an unexpected stage");
        }
        return **shader;
    }

    ShaderAsset& asset = std::get<ShaderAsset>(source);
    ApplyShaderConstantLayoutDefines(asset, argument_metadata, impl->device.GetShaderPlatform());
    return CompileShader(shader_type, std::move(asset));
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
    GfxPsoCreateInfo&&         pso_info,
    PipelineArgumentMetadata&& argument_metadata
) {
    const ShaderStageSource candidates[] = {
        {ST_VERTEX, &vertex_shader_source},
        {ST_HULL, &hull_shader_source},
        {ST_DOMAIN, &domain_shader_source},
        {ST_GEOMETRY, &geometry_shader_source},
        {ST_AMPLIFICATION, &task_shader_source},
        {ST_MESH, &mesh_shader_source},
        {ST_FRAGMENT, &pixel_shader_source}
    };
    const Array<ShaderStageSource> stage_sources = CollectGraphicsShaderSources(candidates);
    if (const std::string_view error = ValidateGraphicsShaderSources(stage_sources); !error.empty()) {
        LOG_ERROR("Cannot create raster pipeline: {}", error);
        return {};
    }
    const bool uses_tessellation = std::ranges::any_of(stage_sources, [](const ShaderStageSource& stage) {
        return stage.shader_type == ST_HULL;
    });
    const EShaderPlatform platform = device.GetShaderPlatform();
    PipelineShaderInfo pipeline_shader_info{.argument_metadata = std::move(argument_metadata)};
    pipeline_shader_info.shaders.reserve(stage_sources.size());
    for (const ShaderStageSource& stage : stage_sources) {
        ShaderSource source = *stage.source;
        ApplyGraphicsShaderDefines(source, stage.shader_type, platform, uses_tessellation);
        Shader& shader = shader_manager.ResolvePipelineShader(
            std::move(source), stage.shader_type, pipeline_shader_info.argument_metadata
        );
        pipeline_shader_info.shaders.push_back(BuildSingleShaderInfo(shader_manager, shader));
    }
    return device.CreatePipeline(std::move(pso_info), std::move(pipeline_shader_info));
}

#pragma region[ compute pipeline ]

ComputeConstructor::ComputeConstructor(RenderDevice& _device, ShaderAsset&& _asset, ShaderManager& _mgr) :
    compute_shader_source(std::move(_asset)),
    device(_device),
    shader_manager(_mgr) {}

PipelineShaderInfo ComputeConstructor::CompileShaderInfo(PipelineArgumentMetadata&& argument_metadata) {
    PipelineShaderInfo pipeline_shader_info{.argument_metadata = std::move(argument_metadata)};
    Shader& shader = shader_manager.ResolvePipelineShader(
        compute_shader_source, ST_COMPUTE, pipeline_shader_info.argument_metadata
    );
    pipeline_shader_info.shaders.push_back(BuildSingleShaderInfo(shader_manager, shader));
    return pipeline_shader_info;
}

PipelineHandle ComputeConstructor::CreatePipeline(PipelineArgumentMetadata&& argument_metadata) {
    return device.CreatePipeline(CompileShaderInfo(std::move(argument_metadata)));
}

#pragma endregion

} // namespace Moer::Render
