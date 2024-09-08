#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderCommon.h"
#include "rhi/RHI.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"

namespace Moer::Render {
    using std::move;

    struct ShaderManager::Impl {

        Impl(Render::RenderDevice& _device) : device(_device) { ShaderCompiler::Init(); }
        Render::RenderDevice& device;
        ~Impl() = default;

        RasterPipelineConstructor Raster() {
            return RasterPipelineConstructor(device);
        }

        ComputeConstructor Compute(std::string_view _path, std::string_view _entry_name = "main") {
            return ComputeConstructor(device, _path, _entry_name);
        }

        RTConstructor RT() {
            return RTConstructor(device);
        }
    };

    ShaderManager::ShaderManager(Render::RenderDevice& _device) {
        impl = MoerNew(Impl)(_device);
    }

    ShaderManager& ShaderManager::Get() {
        static ShaderManager manager(Render::RenderDevice::Get());
        return manager;
    }
    RenderDevice& ShaderManager::GetDevice() {
        return impl->device;
    }

    RasterPipelineConstructor ShaderManager::Raster() {
        return impl->Raster();
    }
    RasterPipelineConstructor::RasterPipelineConstructor(Render::RenderDevice& _device) : device(_device) {
    }

    RTConstructor ShaderManager::RT() {
        return impl->RT();
    }

    PipelineHandle RasterPipelineConstructor::CreatePipeline(GfxPsoCreateInfo&& _pso_info, Array<std::string_view>&& _hash_values, Array<EShaderArgType>&& _arg_type_values) {
        // get shaders and reflection
        bool b_vs_ps = task_path.Empty() && !vertex_path.Empty() && !pixel_path.Empty();
        bool b_gs    = !geometry_path.Empty();
        bool b_mesh  = !mesh_path.Empty() && !pixel_path.Empty();
        bool b_task  = !task_path.Empty();

        auto target_info       = device.GetShaderPlatform();
        auto get_shader_output = [&](ShaderInfo& _info, EShaderType _type) {
            ShaderCompilerInput input{
                .target_info               = ShaderTargetInfo(_type, target_info),
                .entry_point               = _info.entry_name,
                .relative_source_file_path = _info.path,
                .shader_name               = _info.path,
                .environment               = _info.environment};
            auto output = ShaderCompiler::Compile(std::move(input));
            if (!output.b_succeeded) {
                for (const auto& error : output.errors) {
                    LOG_ERROR("Shader Compile Error: {}", error.data());
                }
            }
            if (output.shader_code.empty()) {
                LOG_ERROR("Shader Compile Error: shader code is empty.");
                assert(false && "Shader Compile Error: shader code is empty.");
            }
            return std::move(output);
        };
        auto get_shader_info = [&](EShaderType _type, ShaderInfo& _info, ShaderCompilerOutput&& _output) {
            return std::move(SingleShaderInfo{
                .name             = _info.path,
                .entry_point      = _info.entry_name,
                .shader_data      = std::move(_output.shader_code),
                .shader_type      = _type,
                .shader_param_map = {std::move(_output.parameter_map.param_map)}});
        };
        PipelineShaderInfo sd_info{.layout_hash = std::move(_hash_values), .arg_types = std::move(_arg_type_values)};
        if (b_vs_ps) {
            auto vert_output  = get_shader_output(vertex_path, ST_VERTEX);
            auto pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);
            if (!b_gs) {
                sd_info.shader_group = ShaderVsPs{.vs = get_shader_info(ST_VERTEX, vertex_path, std::move(vert_output)),
                                                  .ps = get_shader_info(ST_FRAGMENT, pixel_path, std::move(pixel_output))};
            } else {
                auto geo_output      = get_shader_output(geometry_path, ST_GEOMETRY);
                sd_info.shader_group = ShaderVsGsPs{.vs = get_shader_info(ST_VERTEX, vertex_path, std::move(vert_output)),
                                                    .gs = get_shader_info(ST_GEOMETRY, geometry_path, std::move(geo_output)),
                                                    .ps = get_shader_info(ST_FRAGMENT, pixel_path, std::move(pixel_output))};
            }
        }

        if (b_mesh) {
            auto mesh_output  = get_shader_output(mesh_path, ST_MESH);
            auto pixel_output = get_shader_output(pixel_path, ST_FRAGMENT);

            if (!b_task) {
                sd_info.shader_group = ShaderMsPs{.ms = get_shader_info(ST_MESH, mesh_path, std::move(mesh_output)),
                                                  .ps = get_shader_info(ST_FRAGMENT, pixel_path, std::move(pixel_output))};
            } else {
                auto task_output     = get_shader_output(task_path, ST_AMPLIFICATION);
                sd_info.shader_group = ShaderTsMsPs{.ts = get_shader_info(ST_AMPLIFICATION, task_path, std::move(task_output)),
                                                    .ms = get_shader_info(ST_MESH, mesh_path, std::move(mesh_output)),
                                                    .ps = get_shader_info(ST_FRAGMENT, pixel_path, std::move(pixel_output))};
            }
        }

        return device.CreatePipeline(std::move(_pso_info), std::move(sd_info));
    }

#pragma region[ compute pipeline ]

    ComputeConstructor::ComputeConstructor(RenderDevice& _device, std::string_view _path, std::string_view _entry_name) : device(_device), shader_info(_path, _entry_name) {
    }

    PipelineHandle ComputeConstructor::CreatePipeline(Array<std::string_view>&& _hash_values, Array<EShaderArgType>&& _arg_type_values) {
        auto target_info       = device.GetShaderPlatform();
        auto get_shader_output = [&](ShaderInfo& _info, EShaderType _type) {
            ShaderCompilerInput input{
                .target_info               = ShaderTargetInfo(_type, target_info),
                .entry_point               = _info.entry_name,
                .relative_source_file_path = _info.path,
                .shader_name               = _info.path,
                .environment               = _info.environment};

            return ShaderCompiler::Compile(std::move(input));
        };
        auto get_shader_info = [&](EShaderType _type, ShaderInfo& _info, ShaderCompilerOutput&& _output) {
            return std::move(SingleShaderInfo{
                .name             = _info.path,
                .entry_point      = _info.entry_name,
                .shader_data      = std::move(_output.shader_code),
                .shader_type      = _type,
                .shader_param_map = {std::move(_output.parameter_map.param_map)}});
        };

        auto               output = get_shader_output(shader_info, ST_COMPUTE);
        PipelineShaderInfo sd_info{.layout_hash = std::move(_hash_values), .arg_types = std::move(_arg_type_values)};
        sd_info.shader_group = ShaderCs{.cs = get_shader_info(ST_COMPUTE, shader_info, std::move(output))};

        return device.CreatePipeline(std::move(sd_info));
    }

#pragma endregion

}// namespace Moer::Render