#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "ShaderPipeline.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "serialize/Serializer.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include <condition_variable>
#include <filesystem>
#include <string_view>
#include <type_traits>

namespace Moer::Render {

struct ShaderInfo {
    std::string               path;
    std::string               entry_name;
    ShaderCompilerEnvironment environment;

    ShaderInfo(
        std::string               _path,
        std::string               _entry_name  = "main",
        ShaderCompilerEnvironment _environment = {}
    ) :
        path(_path),
        entry_name(_entry_name),
        environment(_environment) {}
    ShaderInfo() = default;
    bool Empty() const {
        return path.empty();
    }
};

using ShaderAssetOrCache = std::variant<ShaderAsset, Shader*>;
struct RasterPipelineConstructor {
    RasterPipelineConstructor(Render::RenderDevice& _device, class ShaderManager&);

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Vertex(Ts... _args) {
        vertex_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }

    RasterPipelineConstructor& Vertex(Shader& _shader) {
        vertex_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Pixel(Ts... _args) {
        pixel_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }

    RasterPipelineConstructor& Pixel(Shader& _shader) {
        pixel_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Geometry(Ts... _args) {
        geometry_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }
    RasterPipelineConstructor& Geometry(Shader& _shader) {
        geometry_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Hull(Ts... _args) {
        hull_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }
    RasterPipelineConstructor& Hull(Shader& _shader) {
        hull_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Domain(Ts... _args) {
        domain_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }
    RasterPipelineConstructor& Domain(Shader& _shader) {
        domain_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Mesh(Ts... _args) {
        mesh_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }
    RasterPipelineConstructor& Mesh(Shader& _shader) {
        mesh_path = &_shader;
        return *this;
    }

    template<typename... Ts>
        requires std::is_constructible<ShaderAsset, Ts...>::value
    RasterPipelineConstructor& Task(Ts... _args) {
        task_path = ShaderAsset(std::forward<Ts>(_args)...);
        return *this;
    }

    RasterPipelineConstructor& Task(Shader& _shader) {
        task_path = &_shader;
        return *this;
    }

    template<typename TPipeline>
        requires std::is_base_of_v<RasterPipeline, TPipeline>
    TPipeline Build(GfxPsoCreateInfo&& _pso_info) {
        auto                    hash_array     = TPipeline::GetHashArray();
        auto                    arg_type_array = TPipeline::GetArgInfoArray();
        Array<std::string_view> hash_values(hash_array.size());
        Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
        std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
        std::memcpy(
            arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo)
        );
        PipelineHandle handle =
            CreatePipeline(std::move(_pso_info), std::move(hash_values), std::move(arg_type_values));
        return TPipeline(handle);
    };

private:
    RENDER_API PipelineHandle CreatePipeline(
        GfxPsoCreateInfo&&        _pso_info,
        Array<std::string_view>&& _hash_values,
        Array<ShaderArgCppInfo>&& _arg_type_values
    );

    ShaderAssetOrCache vertex_path;
    ShaderAssetOrCache pixel_path;
    ShaderAssetOrCache geometry_path;
    ShaderAssetOrCache hull_path;
    ShaderAssetOrCache domain_path;
    ShaderAssetOrCache mesh_path;
    ShaderAssetOrCache task_path;

    Render::RenderDevice& device;
    class ShaderManager&  shader_manager;
};
struct ComputeConstructor {
    template<typename TPipeline>
        requires std::is_base_of_v<ComputePipeline, TPipeline>
    TPipeline Build() {
        auto hash_array     = TPipeline::GetHashArray();
        auto arg_type_array = TPipeline::GetArgInfoArray();

        Array<std::string_view> hash_values(hash_array.size());
        Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
        std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
        std::memcpy(
            arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo)
        );
        PipelineHandle handle = CreatePipeline(std::move(hash_values), std::move(arg_type_values));
        return std::move(TPipeline(handle));
    };

    RENDER_API ComputeConstructor(RenderDevice&, ShaderAsset&& _assert, ShaderManager& _mgr);

    template<typename TPipeline>
        requires std::is_base_of_v<ComputePipeline, TPipeline>
    PipelineShaderInfo CompileShaderInfo() {
        auto hash_array     = TPipeline::GetHashArray();
        auto arg_type_array = TPipeline::GetArgInfoArray();

        Array<std::string_view> hash_values(hash_array.size());
        Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
        std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
        std::memcpy(
            arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo)
        );
        return CompileShaderInfo(std::move(hash_values), std::move(arg_type_values));
    }

private:
    RENDER_API PipelineHandle
    CreatePipeline(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values);
    PipelineShaderInfo
    CompileShaderInfo(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values);

    ShaderAssetOrCache    shader_info;
    Render::RenderDevice& device;
    ShaderManager&        shader_manager;
};

struct RTConstructor {
    RTConstructor&        RayGen(std::string_view _path, std::string_view _entry_name = "main");
    RTConstructor&        Miss(std::string_view _path, std::string_view _entry_name = "main");
    RTConstructor&        HitGroup(std::string_view _path, std::string_view _entry_name = "main");
    RTConstructor&        Callable(std::string_view _path, std::string_view _entry_name = "main");
    Render::RenderDevice& device;
};

using ShaderCodeMap = Moer::UnorderedMap<uint, ShaderEntry>;

struct TypedShaderCache {
    UnorderedMap<ShaderCompilerInput, ShaderEntryKey>    shader_cache_map;
    UnorderedMap<ShaderEntryKey, SharedPtr<ShaderEntry>> shader_entry_cache;
    UnorderedMap<ShaderCompilerInput, SharedPtr<Shader>> shader_cache;

    uint64         key_offset = 0;
    ShaderEntryKey AllocateShaderKey(const ShaderCompilerInput& _input) {
        auto it = shader_cache_map.find(_input);
        if (it != shader_cache_map.end()) {
            return it->second;
        }
        auto key                 = ShaderEntryKey{.hash = key_offset++};
        shader_cache_map[_input] = key;
        return key;
    }

    OutputStream& operator<<(OutputStream& _stream) const {
        _stream << shader_cache_map << shader_entry_cache << shader_cache;
        return _stream;
    }
    InputStream& operator>>(InputStream& _stream) {
        _stream >> shader_cache_map >> shader_entry_cache >> shader_cache;
        return _stream;
    }
};

struct ShaderResourcesCache {
    StaticArray<TypedShaderCache, ST_Num> code_cache;

    bool HasCache(const ShaderCompilerInput& _input) const;

    void RegisterCache(const ShaderCompilerInput& _input, ShaderCompilerOutput&& _output);

    std::pair<Shader*, bool> TryGetShader(const ShaderCompilerInput& _input) {
        auto& cache = code_cache[_input.target_info.shader_type].shader_cache;
        auto  it    = cache.find(_input);
        if (it != cache.end()) {
            Shader* shader = it->second.get();
            if (!shader->source_dependencies.empty() && !ValidateDependencies(*shader)) {
                cache.erase(it);
                return {nullptr, false};
            }
            return {shader, true};
        }
        return {nullptr, false};
    }

    // 检查 shader 的所有源文件依赖的时间戳是否与编译时一致。
    // 任何一个文件被修改过（时间戳不同），则返回 false 表示缓存过期。
    static bool ValidateDependencies(const Shader& shader) {
        for (const auto& dep : shader.source_dependencies) {
            try {
                auto current_ts =
                    std::filesystem::last_write_time(dep.path).time_since_epoch().count();
                if (current_ts != dep.timestamp) {
                    return false;
                }
            } catch (...) {
                return false;
            }
        }
        return true;
    }

    ShaderEntry& GetShaderEntry(const Shader& _shader) {
        assert(code_cache[_shader.type].shader_entry_cache.contains(_shader.shader_key));
        return *code_cache[_shader.type].shader_entry_cache[_shader.shader_key];
    }

    OutputStream& operator<<(OutputStream& _stream) const {
        _stream << code_cache;
        return _stream;
    }

    InputStream& operator>>(InputStream& _stream) {
        _stream >> code_cache;
        return _stream;
    }
};

class RENDER_API ShaderManager {
public:
    friend RasterPipelineConstructor;
    friend ComputeConstructor;
    friend RTConstructor;

    ShaderManager(Render::RenderDevice& _device);
    RasterPipelineConstructor Raster();
    template<typename TPipeline>
    TPipeline Compute(std::string_view _path, std::string_view _entry_name = "main") {
        return std::move(
            ComputeConstructor(GetDevice(), ShaderAsset(_path, _entry_name), *this).Build<TPipeline>()
        );
    }

    template<typename TPipeline, is_shader_mutation TMacro>
    TPipeline Compute(std::string_view _path, TMacro _mut, std::string_view _entry_name = "main") {
        return std::move(
            ComputeConstructor(GetDevice(), ShaderAsset(_path, _entry_name, _mut), *this).Build<TPipeline>()
        );
    }
    RTConstructor Raytracing();

    template<is_shader_mutation TMacro = TShaderMutationSet<>>
    Shader& CompileShader(
        EShaderType      _type,
        std::string_view _path,
        TMacro           _mut        = {},
        std::string_view _entry_name = "main"
    ) {
        return CompileShader(_type, ShaderAsset(_path, _entry_name, _mut));
    }

    template<is_shader_mutation TMacro = TShaderMutationSet<>>
    Shader& CompileVertexShader(
        std::string_view _path,
        VertexFactory*   _factory,
        TMacro           _mut        = {},
        std::string_view _entry_name = "main"
    ) {
        return CompileShader(ST_VERTEX, {_path, _entry_name, _factory, _mut});
    }

    Shader& CompileVertexShader(
        std::string_view                 _path,
        VertexFactory*                   _factory,
        std::string_view                 _entry_name,
        const ShaderCompilerEnvironment& _base_env,
        uint                             _mutation_id
    ) {
        ShaderAsset asset(_path, _entry_name, _factory);
        asset.environment.Merge(_base_env);
        asset.mutation_id = _mutation_id;
        return CompileShader(ST_VERTEX, std::move(asset));
    }

    ShaderEntry& GetShaderEntry(const Shader& _shader) {
        return shader_resources_cache.GetShaderEntry(_shader);
    }

    struct Impl;
    friend Impl;

public:
    static ShaderManager& Get();
    static void           ShutDown();

private:
    Shader& CompileShader(EShaderType _type, ShaderAsset&& _asset);
    void    DumpCache(std::filesystem::path _path);
    void    LoadCache(std::filesystem::path _path);

private:
    Render::RenderDevice& GetDevice();
    Impl*                 impl;
    ShaderResourcesCache  shader_resources_cache;
};
} // namespace Moer::Render
#endif