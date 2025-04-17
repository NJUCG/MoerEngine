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
#include <condition_variable>
#include <string_view>
#include <type_traits>

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
            auto                    arg_type_array = TPipeline::GetArgInfoArray();
            Array<std::string_view> hash_values(hash_array.size());
            Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
            std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
            std::memcpy(arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo));
            PipelineHandle handle = CreatePipeline(std::move(_pso_info), std::move(hash_values), std::move(arg_type_values));
            return TPipeline(handle);
        };

    private:
        RENDER_API PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values);

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
            auto arg_type_array = TPipeline::GetArgInfoArray();

            Array<std::string_view> hash_values(hash_array.size());
            Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
            std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
            std::memcpy(arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo));
            PipelineHandle handle = CreatePipeline(std::move(hash_values), std::move(arg_type_values));
            return std::move(TPipeline(handle));
        };

        RENDER_API ComputeConstructor(RenderDevice&, ShaderAsset&& _assert);

        template<typename TPipeline>
            requires std::is_base_of_v<ComputePipeline, TPipeline>
        PipelineShaderInfo CompileShaderInfo() {
            auto hash_array     = TPipeline::GetHashArray();
            auto arg_type_array = TPipeline::GetArgInfoArray();

            Array<std::string_view> hash_values(hash_array.size());
            Array<ShaderArgCppInfo> arg_type_values(hash_array.size());
            std::memcpy(hash_values.data(), hash_array.data(), hash_array.size() * sizeof(std::string_view));
            std::memcpy(arg_type_values.data(), arg_type_array.data(), arg_type_array.size() * sizeof(ShaderArgCppInfo));
            return CompileShaderInfo(std::move(hash_values), std::move(arg_type_values));
        }

    private:
        RENDER_API PipelineHandle CreatePipeline(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values);
        PipelineShaderInfo        CompileShaderInfo(Array<std::string_view>&& _hash_values, Array<ShaderArgCppInfo>&& _arg_type_values);

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
            return std::move(ComputeConstructor(GetDevice(), ShaderAsset(_path, _entry_name)).Build<TPipeline>());
        }

        template<typename TPipeline, is_shader_mutation TMacro>
        TPipeline Compute(std::string_view _path, TMacro _mut, std::string_view _entry_name = "main") {
            return std::move(ComputeConstructor(GetDevice(), ShaderAsset(_path, _entry_name, _mut)).Build<TPipeline>());
        }
        RTConstructor Raytracing();

        struct Impl;
        friend Impl;

    public:
        static ShaderManager& Get();
        static void           ShutDown();

    private:
        Render::RenderDevice& GetDevice();
        Impl*                 impl;
    };
}// namespace Moer::Render
#endif