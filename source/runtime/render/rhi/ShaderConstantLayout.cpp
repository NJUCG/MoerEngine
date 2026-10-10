#include "rhi/ShaderConstantLayout.h"

#include <stdexcept>
#include <string>

namespace Moer::Render {
namespace {

const ShaderConstantArgumentLayout*
FindCppConstantLayout(const PipelineArgumentMetadata& cpp_metadata, std::string_view parameter_name) {
    const auto cpp_arguments = cpp_metadata.GetArguments();
    for (const auto& cpp_layout : cpp_metadata.GetConstantLayouts()) {
        if (cpp_arguments[cpp_layout.argument_index].name == parameter_name) {
            return &cpp_layout;
        }
    }
    return nullptr;
}

void ValidateConstantBindingTypes(
    const PipelineArgumentMetadata& cpp_metadata,
    const ShaderParametersInfoMap&  shader_parameters
) {
    const auto cpp_arguments = cpp_metadata.GetArguments();
    for (const auto& cpp_layout : cpp_metadata.GetConstantLayouts()) {
        const auto constant_name       = cpp_arguments[cpp_layout.argument_index].name;
        const auto reflected_parameter = shader_parameters.reflect_map.find(std::string(constant_name));
        if (reflected_parameter == shader_parameters.reflect_map.end()) {
            continue;
        }
        if (reflected_parameter->second.GetSpirv().TryGetConstant() == nullptr) {
            throw std::invalid_argument(
                "Shader constant argument has a resource binding: " + std::string(constant_name)
            );
        }
    }
}

void ValidateConstantLayoutMatch(
    const ShaderConstantArgumentLayout& cpp_layout,
    const ReflectParamInfo::Constant&   shader_constant,
    std::string_view                    constant_name,
    std::string_view                    shader_name
) {
    if (shader_constant.offset == cpp_layout.byte_offset && shader_constant.size != 0 &&
        shader_constant.size <= cpp_layout.byte_size) {
        return;
    }
    throw std::invalid_argument(
        "Shader constant layout mismatch for '" + std::string(constant_name) + "' in shader '" +
        std::string(shader_name) + "': expected offset=" + std::to_string(cpp_layout.byte_offset) +
        ", available size=" + std::to_string(cpp_layout.byte_size) + ", reflected offset=" +
        std::to_string(shader_constant.offset) + ", size=" + std::to_string(shader_constant.size)
    );
}

// Returns whether this shader uses constants; validation failures throw.
bool ValidateShaderConstantUsage(
    const PipelineArgumentMetadata& cpp_metadata,
    const ShaderParametersInfoMap&  shader_parameters,
    std::string_view                shader_name
) {
    bool uses_constants = false;
    for (const auto& [parameter_name, parameter] : shader_parameters.reflect_map) {
        const auto* shader_constant = parameter.GetSpirv().TryGetConstant();
        if (shader_constant == nullptr || !shader_constant->custom_flag.active) {
            continue;
        }
        const auto* cpp_layout = FindCppConstantLayout(cpp_metadata, parameter_name);
        if (cpp_layout == nullptr) {
            throw std::invalid_argument(
                "Undeclared active constant '" + std::string(parameter_name) + "' in shader '" +
                std::string(shader_name) + "'"
            );
        }

        ValidateConstantLayoutMatch(*cpp_layout, *shader_constant, parameter_name, shader_name);
        uses_constants = true;
    }
    return uses_constants;
}

} // namespace

uint ValidatePipelineConstants(const PipelineShaderInfo& pipeline_shader_info, uint max_constant_byte_size) {
    const auto& cpp_metadata     = pipeline_shader_info.argument_metadata;
    uint        upload_byte_size = 0;
    for (const auto& shader : pipeline_shader_info.shaders) {
        if (shader.shader_param_map == nullptr) {
            continue;
        }
        const auto& shader_parameters = *shader.shader_param_map;
        ValidateConstantBindingTypes(cpp_metadata, shader_parameters);
        if (ValidateShaderConstantUsage(cpp_metadata, shader_parameters, shader.name)) {
            upload_byte_size = cpp_metadata.GetConstantByteSize();
        }
    }

    if (upload_byte_size > max_constant_byte_size) {
        throw std::invalid_argument(
            "Pipeline constants exceed the device limit: " + std::to_string(upload_byte_size) + " > " +
            std::to_string(max_constant_byte_size)
        );
    }
    return upload_byte_size;
}

} // namespace Moer::Render
