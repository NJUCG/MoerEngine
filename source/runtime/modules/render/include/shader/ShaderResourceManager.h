#ifndef MOERENGINE_SHADER_RESOURCE_MANAGER_H
#define MOERENGINE_SHADER_RESOURCE_MANAGER_H

#include "ShaderPipeline.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderResource.h"
#include <condition_variable>
#include <string_view>
class GlobalShaderCache {
public:
    static GlobalShaderCache& GetInstance() {
        static GlobalShaderCache cache;
        return cache;
    }
    GlobalShaderCache();
    ~GlobalShaderCache();

    const ShaderCompilerOutput* FindShaderCache(EShaderPlatform platform, const ShaderResourceKey& key) const;

private:
    friend class ShaderResourceManager;
    void Load();
    void Dump();
    void UpdateOutput(Moer::Array<ShaderCompilerOutput*>& outputs);

private:
    struct Impl;
    Impl* impl;
};

namespace Moer {
    struct ShaderBlob {
        EShaderType     type;
        EShaderPlatform platform;
        Array<uint8_t>  blob_data;
    };
    struct ShaderEntry {
        // shader may have mutations
        Array<ShaderBlob> blobs;
    };
    struct ShaderEntryKey {
        uint64 hash;

        bool operator==(const ShaderEntryKey& _rhs) const noexcept {
            return hash == _rhs.hash;
        }
    };
};// namespace Moer
static bool operator==(const Moer::ShaderEntryKey& _lhs, const Moer::ShaderEntryKey& _rhs) noexcept {
    return _lhs.hash == _rhs.hash;
}
namespace std {
    template<>
    struct hash<Moer::ShaderEntryKey> {
        size_t operator()(const Moer::ShaderEntryKey& _key) const {
            return _key.hash;
        }
    };
}// namespace std

class RENDER_API ShaderResourceManager {
public:
    static void                   Init(EShaderPlatform platform);
    static void                   ShutDown();
    static ShaderResourceManager& GetInstance();

    template<typename ShaderType>
        requires std::is_base_of_v<Shader, ShaderType>
    RHIShaderRef GetShader(const uint32_t _mutation_id) {
        const ShaderMetaType& meta_type = ShaderType::GetMetaType();

        if constexpr (ShaderType::TMutationSet::mutation_count == 0) {
            return GetShader(meta_type, 0);
        } else
            return GetShader(meta_type, _mutation_id);
    }
    template<typename ShaderType>
        requires std::is_base_of_v<Shader, ShaderType>
    RHIShaderRef GetShader() {
        static_assert(ShaderType::TMutationSet::mutation_count == 1, "ShaderType should not have any mutation");
        const ShaderMetaType& meta_type = ShaderType::GetMetaType();

        return GetShader(meta_type, 0);
    }

    ShaderTypeResourceMap& GetShaderTypeMap() {
        return *type_resources;
    }

    void    PrepareGlobalShaderResources();
    Shader* GetShader(const ShaderMetaType& _meta_type);

    Moer::ShaderBlob GetShaderBlob(std::string_view _path, EShaderType _type);

private:
    friend class ShaderCompiler;
    ShaderResourceMap& GetShaderResourceMap() {
        return *shader_resources;
    }
    RHIShaderRef GetShader(const ShaderMetaType& _meta_type, uint32_t _mutation_id);
    friend Shader;
    ShaderResourceManager();
    ShaderTypeResourceMap* type_resources;
    ShaderResourceMap*     shader_resources;

    Moer::UnorderedMap<Moer::ShaderEntryKey, Moer::ShaderBlob> shader_cache;
};

namespace Moer::Render {

    struct ShaderInfo {
        std::string_view          path;
        std::string_view          entry_name;
        ShaderCompilerEnvironment environment;

        ShaderInfo(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {})
            : path(_path), entry_name(_entry_name), environment(_environment) {
        }
        ShaderInfo() = default;
        bool Empty() const {
            return path.empty();
        }
    };
    struct RasterPipelineConstructor {
        RasterPipelineConstructor(Render::RenderDevice& _device);
        RasterPipelineConstructor& Vertex(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            vertex_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Pixel(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            pixel_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Geometry(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            geometry_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Hull(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            hull_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Domain(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            domain_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Mesh(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            mesh_path = {_path, _entry_name, _environment};
            return *this;
        }
        RasterPipelineConstructor& Task(std::string_view _path, std::string_view _entry_name = "main", ShaderCompilerEnvironment _environment = {}) {
            task_path = {_path, _entry_name, _environment};
            return *this;
        }

        template<typename TPipeline>
            requires std::is_base_of_v<RasterPipeline, TPipeline>
        TPipeline Build(GfxPsoCreateInfo&& _pso_info) {
            auto                    hash_array     = TPipeline::GetHashArray();
            auto                    arg_type_array = TPipeline::GetArgTypeArray();
            Array<std::string_view> hash_values(hash_array.size());
            Array<EShaderArgType>   arg_type_values(hash_array.size());
            std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
            std::memcpy(arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(EShaderArgType));
            PipelineHandle handle = CreatePipeline(std::move(_pso_info), std::move(hash_values), std::move(arg_type_values));
            return TPipeline(handle);
        };

    private:
        RENDER_API PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, Array<std::string_view>&& _hash_values, Array<EShaderArgType>&& _arg_type_values);

        ShaderInfo vertex_path;
        ShaderInfo pixel_path;
        ShaderInfo geometry_path;
        ShaderInfo hull_path;
        ShaderInfo domain_path;
        ShaderInfo mesh_path;
        ShaderInfo task_path;

        Render::RenderDevice& device;
    };
    struct ComputeConstructor {
        template<typename TPipeline>
            requires std::is_base_of_v<ComputePipeline, TPipeline>
        TPipeline Build() {
            auto hash_array     = TPipeline::GetHashArray();
            auto arg_type_array = TPipeline::GetArgTypeArray();

            Array<std::string_view> hash_values(hash_array.size());
            Array<EShaderArgType>   arg_type_values(hash_array.size());
            std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
            std::memcpy(arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(EShaderArgType));
            PipelineHandle handle = CreatePipeline(std::move(hash_values), std::move(arg_type_values));
            return TPipeline();
        };
        ComputeConstructor(RenderDevice&, std::string_view _path, std::string_view _entry_name);

    private:
        PipelineHandle CreatePipeline(Array<std::string_view>&& _hash_values, Array<EShaderArgType>&& _arg_type_values);

        ShaderInfo            shader_info;
        Render::RenderDevice& device;
    };

    struct RTConstructor {
        RTConstructor&        RayGen(std::string_view _path, std::string_view _entry_name = "main");
        RTConstructor&        Miss(std::string_view _path, std::string_view _entry_name = "main");
        RTConstructor&        HitGroup(std::string_view _path, std::string_view _entry_name = "main");
        RTConstructor&        Callable(std::string_view _path, std::string_view _entry_name = "main");
        Render::RenderDevice& device;
    };
    class RENDER_API ShaderManager {
    public:
        ShaderManager(Render::RenderDevice& _device);
        RasterPipelineConstructor Raster();
        template<typename TPipeline>
        TPipeline Compute(std::string_view _path, std::string_view _entry_name = "main") {
            return ComputeConstructor(GetDevice(), _path, _entry_name).Build<TPipeline>();
        }
        RTConstructor Raytracing();

        struct Impl;
        friend Impl;

    public:
        static void           Register(ShaderManager&);
        static ShaderManager& Get();

    private:
        Render::RenderDevice& GetDevice();
        Impl*                 impl;
    };
}// namespace Moer::Render
#endif