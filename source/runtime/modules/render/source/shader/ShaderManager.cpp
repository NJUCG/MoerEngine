#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "rhi/RHI.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render {
    struct ShaderManager::Impl {

        Impl(Render::RenderDevice& _device) : device(_device) {}
        Render::RenderDevice& device;
        ~Impl() = default;

        RasterPipelineConstructor Raster() {
            return RasterPipelineConstructor(device);
        }

        ComputeConstructor Compute(std::string_view _path, std::string_view _entry_name = "main") {
            return ComputeConstructor(_path, _entry_name);
        }

        RTConstructor RT() {
            return RTConstructor(device);
        }
    };

    ShaderManager::ShaderManager(Render::RenderDevice& _device) {
        impl = std::move(UniquePtr<Impl>(MoerNew(Impl)(_device)));
    }

    RasterPipelineConstructor ShaderManager::Raster() {
        return impl->Raster();
    }

    RTConstructor ShaderManager::RT() {
        return impl->RT();
    }

    PipelineHandle RasterPipelineConstructor::CreatePipeline(GfxPsoCreateInfo&& _pso_info, Array<std::string_view>& _hash_values) {
        // get shaders and reflection
        bool b_vs_ps = task_path.Empty() && !vertex_path.Empty() && !pixel_path.Empty();
        bool b_gs    = !geometry_path.Empty();
        bool b_mesh  = !mesh_path.Empty() && !pixel_path.Empty();
        bool b_task  = !task_path.Empty();

        auto target_info       = device.GetShaderTargetInfo();
        auto get_shader_output = [&](ShaderInfo& _info) {
            ShaderCompilerInput input{
                .target_info               = target_info,
                .entry_point               = _info.entry_name,
                .relative_source_file_path = _info.path,
                .shader_name               = _info.path,
                .environment               = _info.environment};

            return ShaderCompiler::Compile(std::move(input));
        };
        auto get_shader_info = [&](EShaderType _type, ShaderCompilerOutput&& _output) {
            return std::move(SingleShaderInfo{.shader_data      = std::move(_output.shader_code),
                                              .shader_type      = _type,
                                              .shader_param_map = std::move(_output.parameter_map.param_map)});
        };
        PipelineShaderInfo sd_info{.layout_hash = _hash_values};
        if (b_vs_ps) {
            auto vert_output  = get_shader_output(vertex_path);
            auto pixel_output = get_shader_output(pixel_path);
            if (!b_gs) {
                sd_info.shader_group = ShaderVsPs{.vs = get_shader_info(ST_VERTEX, std::move(vert_output)),
                                                  .ps = get_shader_info(ST_FRAGMENT, std::move(pixel_output))};
            } else {
                auto geo_output      = get_shader_output(geometry_path);
                sd_info.shader_group = ShaderVsGsPs{.vs = get_shader_info(ST_VERTEX, std::move(vert_output)),
                                                    .gs = get_shader_info(ST_GEOMETRY, std::move(geo_output)),
                                                    .ps = get_shader_info(ST_FRAGMENT, std::move(pixel_output))};
            }
        }

        if (b_mesh) {
            auto mesh_output  = get_shader_output(mesh_path);
            auto pixel_output = get_shader_output(pixel_path);

            if (!b_task) {
                sd_info.shader_group = ShaderMsPs{.ms = get_shader_info(ST_MESH, std::move(mesh_output)),
                                                  .ps = get_shader_info(ST_FRAGMENT, std::move(pixel_output))};
            } else {
                auto task_output     = get_shader_output(task_path);
                sd_info.shader_group = ShaderTsMsPs{.ts = get_shader_info(ST_AMPLIFICATION, std::move(task_output)),
                                                    .ms = get_shader_info(ST_MESH, std::move(mesh_output)),
                                                    .ps = get_shader_info(ST_FRAGMENT, std::move(pixel_output))};
            }
        }

        return device.CreatePipeline(std::move(_pso_info), std::move(sd_info));
    }

}// namespace Moer::Render