#include "misc/Types.h"
#include "shader/ShaderResourceManager.h"

#include "rhi/ShaderStageUtils.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Moer::Render {

namespace {

void ApplyShaderConstantLayoutDefines(
    ShaderAsset&                    asset,
    const PipelineArgumentMetadata& metadata,
    EShaderPlatform                 platform
) {
    if (platform == SP_WIN_D3D_SM6 && metadata.GetConstantLayouts().size() > 1) {
        throw std::invalid_argument("D3D12 does not yet support multiple logical constant arguments");
    }
    asset.environment.SetDefine("MOER_PC_LAYOUT_VERSION", uint32(1));
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
        .shader_data        = std::span<uint8>(entry.blob_data.data(), entry.blob_data.size()),
        .shader_type        = shader.type,
        .shader_param_map   = &shader.reflection,
        .compute_local_size = shader.reflection.compute_local_size
    };
}

} // namespace

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
    ApplyShaderConstantLayoutDefines(asset, argument_metadata, GetDevice().GetShaderPlatform());
    return CompileShader(shader_type, std::move(asset));
}

RasterPipelineConstructor::RasterPipelineConstructor(Render::RenderDevice& _device, ShaderManager& _manager) :
    device(_device),
    shader_manager(_manager) {}

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

} // namespace Moer::Render
