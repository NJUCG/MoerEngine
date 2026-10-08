#include "rhi/ShaderStageUtils.h"

#include <array>

namespace Moer::Render {

std::string_view ValidateGraphicsShaderStages(std::span<const SingleShaderInfo> shaders) {
    std::array<bool, ST_Num> has_stage{};
    for (const auto& shader : shaders) {
        switch (shader.shader_type) {
            case ST_VERTEX:
            case ST_HULL:
            case ST_DOMAIN:
            case ST_GEOMETRY:
            case ST_FRAGMENT:
            case ST_AMPLIFICATION:
            case ST_MESH:
                break;
            default:
                return "Graphics pipeline contains a non-graphics shader stage.";
        }
        if (has_stage[shader.shader_type]) {
            return "Graphics pipeline contains a duplicate shader stage.";
        }
        has_stage[shader.shader_type] = true;
    }

    if (has_stage[ST_VERTEX] == has_stage[ST_MESH]) {
        return "Graphics pipeline requires exactly one of Vertex or Mesh shader.";
    }
    if (has_stage[ST_AMPLIFICATION] && !has_stage[ST_MESH]) {
        return "Task shader requires a Mesh shader.";
    }
    if (has_stage[ST_MESH] && (has_stage[ST_HULL] || has_stage[ST_DOMAIN] || has_stage[ST_GEOMETRY])) {
        return "Mesh pipeline cannot contain Hull, Domain or Geometry shaders.";
    }
    if (has_stage[ST_HULL] != has_stage[ST_DOMAIN]) {
        return "Tessellation requires both Hull and Domain shaders.";
    }
    if (!has_stage[ST_FRAGMENT]) {
        return "Graphics pipelines currently require a Pixel shader.";
    }
    if (has_stage[ST_HULL] && has_stage[ST_GEOMETRY]) {
        return "Tessellation with a Geometry shader is not currently supported.";
    }
    return {};
}

std::string_view ValidateComputeShaderStages(std::span<const SingleShaderInfo> shaders) {
    if (shaders.size() != 1 || shaders.front().shader_type != ST_COMPUTE) {
        return "Compute pipeline requires exactly one Compute shader.";
    }
    return {};
}

const SingleShaderInfo* FindShaderStage(std::span<const SingleShaderInfo> shaders, EShaderType shader_type) {
    for (const auto& shader : shaders) {
        if (shader.shader_type == shader_type) {
            return &shader;
        }
    }
    return nullptr;
}

Array<const SingleShaderInfo*> GetGraphicsShadersInStageOrder(std::span<const SingleShaderInfo> shaders) {
    // EShaderType values are serialized and are not ordered by pipeline stage.
    constexpr EShaderType k_stage_order[] = {
        ST_VERTEX, ST_HULL, ST_DOMAIN, ST_GEOMETRY, ST_AMPLIFICATION, ST_MESH, ST_FRAGMENT
    };
    Array<const SingleShaderInfo*> stages;
    stages.reserve(shaders.size());
    for (const auto shader_type : k_stage_order) {
        if (const auto* shader = FindShaderStage(shaders, shader_type)) {
            stages.push_back(shader);
        }
    }
    return stages;
}

} // namespace Moer::Render
