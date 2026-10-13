#include "shader/ShaderResourceManager.h"

#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "shader/ShaderCompiler.h"

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Moer::Render {

namespace {

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

} // namespace

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

RenderDevice& ShaderManager::GetDevice() {
    return impl->device;
}

RasterPipelineConstructor ShaderManager::Raster() {
    return impl->Raster();
}

RTConstructor ShaderManager::Raytracing() {
    return impl->Raytracing();
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

} // namespace Moer::Render
