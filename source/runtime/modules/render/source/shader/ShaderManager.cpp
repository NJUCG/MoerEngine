#include "config/ConfigManager.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "serialize/Serializer.h"
#include "shader/ShaderCommon.h"
#include "rhi/RHI.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include <ostream>
#include <string_view>
#include <fstream>

namespace Moer::Render {
    using std::move;

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

            cache.shader_entry_cache[key] = MakeShared<ShaderEntry>((EShaderType)_input.target_info.shader_type,
                                                                    (EShaderPlatform)_input.target_info.shader_platform,
                                                                    std::move(_output.shader_code));

            cache.shader_cache[_input] = MakeShared<Shader>(
                _output.parameter_map,
                _output.mutation_id,
                (EShaderType)_input.target_info.shader_type,
                StaticArray<uint64, 2>{_output.compiled_hash1, _output.compiled_hash2},
                _input.shader_name_hash,
                _input.entry_point,
                _input.relative_source_file_path,
                key);
        }
    }

#pragma endregion

    struct ShaderManager::Impl {

        Impl(Render::RenderDevice& _device, ShaderManager& _manager) : device(_device), manager(_manager) { ShaderCompiler::Init(); }
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
            return;
        }

        std::ifstream fs(_path, std::ios::binary);
        InputStream   stream(fs);
        //MARK. not implemented
        // stream >> shader_resources_cache;
    }

    Shader& ShaderManager::CompileShader(EShaderType _type, ShaderAsset&& _asset) {

        ShaderCompilerInput input{
            .target_info               = ShaderTargetInfo(_type, impl->device.GetShaderPlatform()),
            .mutation_id               = _asset.mutation_id,
            .entry_point               = _asset.entry_name,
            .relative_source_file_path = _asset.path,
            .shader_name               = _asset.path,
            .shader_name_hash          = GetHash(_asset.path),
            .environment               = std::move(_asset.environment)};

        auto it = shader_resources_cache.TryGetShader(input);
        if (it.first != nullptr) {
            return *it.first;
        }
        auto&& output = ShaderCompiler::Compile(std::move(input));
        if (!output.b_succeeded) {
            for (const auto& error : output.errors) {
                LOG_ERROR("Shader Compile Error: {}", error.data());
            }
            assert(false && std::format("Shader Compile Error, FileName: {}", input.relative_source_file_path).c_str());
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
    RasterPipelineConstructor::RasterPipelineConstructor(Render::RenderDevice& _device, ShaderManager& _manager) : device(_device), shader_manager(_manager) {
    }

    RTConstructor ShaderManager::Raytracing() {
        return impl->Raytracing();
    }

    PipelineHandle RasterPipelineConstructor::CreatePipeline(GfxPsoCreateInfo&& _pso_info, Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values) {
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
            return std::move(SingleShaderInfo{
                .name             = _output.shader_path,
                .entry_point      = _output.entry_name,
                .shader_data      = std::span<uint8_t>(entry.blob_data.data(), entry.blob_data.size()),
                .shader_type      = _type,
                .shader_param_map = &_output.reflection});
        };
        PipelineShaderInfo sd_info{.layout_hash = std::move(_hash_values), .arg_cpp_info = std::move(_arg_type_values)};
        if (b_vs_ps) {
            auto& vert_output  = get_shader_output(vertex_path, ST_VERTEX);
            auto& pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);
            if (!b_gs) {
                sd_info.shader_group = ShaderVsPs{.vs = get_shader_info(ST_VERTEX, vert_output),
                                                  .ps = get_shader_info(ST_FRAGMENT, pixel_output)};
            } else {
                auto& geo_output     = get_shader_output(geometry_path, ST_GEOMETRY);
                sd_info.shader_group = ShaderVsGsPs{.vs = get_shader_info(ST_VERTEX, vert_output),
                                                    .gs = get_shader_info(ST_GEOMETRY, geo_output),
                                                    .ps = get_shader_info(ST_FRAGMENT, pixel_output)};
            }
        }

        if (b_mesh) {
            auto& mesh_output  = get_shader_output(mesh_path, ST_MESH);
            auto& pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);

            if (!b_task) {
                sd_info.shader_group = ShaderMsPs{.ms = get_shader_info(ST_MESH, mesh_output),
                                                  .ps = get_shader_info(ST_FRAGMENT, pixel_output)};
            } else {
                auto task_output     = get_shader_output(task_path, ST_AMPLIFICATION);
                sd_info.shader_group = ShaderTsMsPs{.ts = get_shader_info(ST_AMPLIFICATION, task_output),
                                                    .ms = get_shader_info(ST_MESH, mesh_output),
                                                    .ps = get_shader_info(ST_FRAGMENT, pixel_output)};
            }
        }

        return device.CreatePipeline(std::move(_pso_info), std::move(sd_info));
    }

#pragma region[ compute pipeline ]

    ComputeConstructor::ComputeConstructor(RenderDevice& _device, ShaderAsset&& _asset, ShaderManager& _mgr) : device(_device), shader_manager(_mgr), shader_info(std::move(_asset)) {
    }

    PipelineShaderInfo ComputeConstructor::CompileShaderInfo(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values) {

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
            return std::move(SingleShaderInfo{
                .name             = _output.shader_path,
                .entry_point      = _output.entry_name,
                .shader_data      = std::span<uint8_t>(entry.blob_data.data(), entry.blob_data.size()),
                .shader_type      = _type,
                .shader_param_map = &_output.reflection});
        };

        auto&              output = get_shader_output(shader_info, ST_COMPUTE);
        PipelineShaderInfo sd_info{.layout_hash = std::move(_hash_values), .arg_cpp_info = std::move(_arg_type_values)};
        sd_info.shader_group = ShaderCs{.cs = get_shader_info(ST_COMPUTE, output)};
        return std::move(sd_info);
    }

    PipelineHandle ComputeConstructor::CreatePipeline(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values) {
        return device.CreatePipeline(CompileShaderInfo(std::move(_hash_values), std::move(_arg_type_values)));
    }

#pragma endregion

}// namespace Moer::Render