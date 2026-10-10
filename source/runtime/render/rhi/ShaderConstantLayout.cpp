#include "rhi/ShaderConstantLayout.h"

#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Moer::Render {
namespace {

constexpr uint k_constant_word_byte_size = 4;

void ValidateUniqueArgumentNames(std::span<const PipelineArgumentInfo> arguments) {
    std::unordered_set<std::string_view> seen_names;
    for (const auto& argument : arguments) {
        const bool inserted = seen_names.insert(argument.name).second;
        if (!inserted) {
            throw std::invalid_argument("Duplicate shader argument: " + std::string(argument.name));
        }
    }
}

void ValidateCppConstantLayout(const PipelineShaderInfo& pipeline_shader_info) {
    const auto&              constant_layout = pipeline_shader_info.constant_layout;
    const auto&              arguments       = pipeline_shader_info.arguments;
    std::unordered_set<uint> constant_argument_indices;
    uint64                   previous_argument_end_byte_offset = 0;

    for (const auto& argument_layout : constant_layout.arguments) {
        const uint argument_index = argument_layout.argument_index;
        if (argument_index >= arguments.size() || arguments[argument_index].cpp_info.type != SDA_Constant ||
            !constant_argument_indices.insert(argument_index).second) {
            throw std::invalid_argument("Invalid pipeline constant argument layout");
        }

        const uint64 argument_end_byte_offset =
            uint64(argument_layout.byte_offset) + argument_layout.byte_size;
        const bool is_word_aligned = argument_layout.byte_offset % k_constant_word_byte_size == 0 &&
                                     argument_layout.byte_size % k_constant_word_byte_size == 0;
        if (!is_word_aligned || argument_layout.byte_offset < previous_argument_end_byte_offset ||
            argument_end_byte_offset > constant_layout.byte_size) {
            throw std::invalid_argument("Invalid pipeline constant argument layout");
        }
        previous_argument_end_byte_offset = argument_end_byte_offset;
    }

    for (uint argument_index = 0; argument_index < arguments.size(); ++argument_index) {
        if (arguments[argument_index].cpp_info.type == SDA_Constant &&
            !constant_argument_indices.contains(argument_index)) {
            throw std::invalid_argument(
                "Missing constant layout for argument: " + std::string(arguments[argument_index].name)
            );
        }
    }
    if (constant_layout.byte_size % k_constant_word_byte_size != 0 ||
        (constant_layout.arguments.empty() && constant_layout.byte_size != 0)) {
        throw std::invalid_argument("Invalid pipeline constant data size");
    }
}

const ShaderConstantArgumentLayout*
FindConstantArgumentLayout(const PipelineShaderInfo& pipeline_shader_info, std::string_view constant_name) {
    for (const auto& argument_layout : pipeline_shader_info.constant_layout.arguments) {
        if (pipeline_shader_info.arguments[argument_layout.argument_index].name == constant_name) {
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
    for (const auto& argument_layout : pipeline_shader_info.constant_layout.arguments) {
        const auto constant_name       = pipeline_shader_info.arguments[argument_layout.argument_index].name;
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
    ValidateUniqueArgumentNames(pipeline_shader_info.arguments);
    ValidateCppConstantLayout(pipeline_shader_info);

    bool has_active_constants = false;
    for (const auto& shader : pipeline_shader_info.shaders) {
        const bool shader_has_active_constants =
            ValidateShaderConstantReflection(pipeline_shader_info, shader);
        has_active_constants = has_active_constants || shader_has_active_constants;
    }
    if (!has_active_constants) {
        return 0;
    }

    const uint upload_byte_size = pipeline_shader_info.constant_layout.byte_size;
    if (upload_byte_size == 0 || upload_byte_size > max_constant_byte_size) {
        throw std::invalid_argument(
            "Pipeline constants exceed the device limit: " + std::to_string(upload_byte_size) + " > " +
            std::to_string(max_constant_byte_size)
        );
    }
    return upload_byte_size;
}

} // namespace Moer::Render
