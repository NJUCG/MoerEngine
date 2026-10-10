#include "rhi/ShaderConstantLayout.h"

#include <stdexcept>
#include <string>

namespace Moer::Render {
namespace {

const ShaderConstantArgumentLayout*
FindConstantArgumentLayout(const PipelineShaderInfo& pipeline_shader_info, std::string_view constant_name) {
    const auto& metadata = pipeline_shader_info.argument_metadata;
    for (const auto& argument_layout : metadata.GetConstantLayouts()) {
        if (metadata.GetArguments()[argument_layout.argument_index].name == constant_name) {
            return &argument_layout;
        }
    }
    return nullptr;
}

void ValidateShaderConstantArgumentTypes(
    const PipelineShaderInfo&      pipeline_shader_info,
    const ShaderParametersInfoMap& shader_reflection
) {
    const auto& reflected_parameters = shader_reflection.reflect_map;
    const auto& metadata             = pipeline_shader_info.argument_metadata;
    for (const auto& argument_layout : metadata.GetConstantLayouts()) {
        const auto constant_name       = metadata.GetArguments()[argument_layout.argument_index].name;
        const auto reflected_parameter = reflected_parameters.find(std::string(constant_name));
        if (reflected_parameter != reflected_parameters.end() &&
            !std::holds_alternative<ReflectParamInfo::Constant>(
                reflected_parameter->second.spirv.resources.data
            )) {
            throw std::invalid_argument(
                "Shader constant argument has a resource binding: " + std::string(constant_name)
            );
        }
    }
}

bool ValidateShaderConstantReflection(
    const PipelineShaderInfo& pipeline_shader_info,
    const SingleShaderInfo&   shader
) {
    if (shader.shader_param_map == nullptr) {
        return false;
    }
    ValidateShaderConstantArgumentTypes(pipeline_shader_info, *shader.shader_param_map);

    bool has_active_constants = false;
    for (const auto& [constant_name, reflected_parameter] : shader.shader_param_map->reflect_map) {
        if (constant_name == ReflectParamInfo::bdls_name) {
            continue;
        }
        const auto* reflected_constant =
            std::get_if<ReflectParamInfo::Constant>(&reflected_parameter.spirv.resources.data);
        if (reflected_constant == nullptr || !reflected_constant->custom_flag.active) {
            continue;
        }
        has_active_constants = true;

        const auto* declared_argument_layout =
            FindConstantArgumentLayout(pipeline_shader_info, constant_name);
        if (declared_argument_layout == nullptr) {
            throw std::invalid_argument(
                "Undeclared active constant '" + std::string(constant_name) + "' in shader '" +
                std::string(shader.name) + "'"
            );
        }

        const bool offsets_match = reflected_constant->offset == declared_argument_layout->byte_offset;
        const bool has_valid_reflected_size =
            reflected_constant->size != 0 && reflected_constant->size <= declared_argument_layout->byte_size;
        if (!offsets_match || !has_valid_reflected_size) {
            throw std::invalid_argument(
                "Shader constant layout mismatch for '" + std::string(constant_name) + "' in shader '" +
                std::string(shader.name) +
                "': expected offset=" + std::to_string(declared_argument_layout->byte_offset) +
                ", available size=" + std::to_string(declared_argument_layout->byte_size) +
                ", reflected offset=" + std::to_string(reflected_constant->offset) +
                ", size=" + std::to_string(reflected_constant->size)
            );
        }
    }
    return has_active_constants;
}

} // namespace

uint ValidatePipelineConstants(const PipelineShaderInfo& pipeline_shader_info, uint max_constant_byte_size) {
    bool has_active_constants = false;
    for (const auto& shader : pipeline_shader_info.shaders) {
        const bool shader_has_active_constants =
            ValidateShaderConstantReflection(pipeline_shader_info, shader);
        has_active_constants = has_active_constants || shader_has_active_constants;
    }
    if (!has_active_constants) {
        return 0;
    }

    const uint upload_byte_size = pipeline_shader_info.argument_metadata.GetConstantByteSize();
    if (upload_byte_size == 0 || upload_byte_size > max_constant_byte_size) {
        throw std::invalid_argument(
            "Pipeline constants exceed the device limit: " + std::to_string(upload_byte_size) + " > " +
            std::to_string(max_constant_byte_size)
        );
    }
    return upload_byte_size;
}

} // namespace Moer::Render
