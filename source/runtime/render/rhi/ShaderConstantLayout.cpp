#include "rhi/ShaderConstantLayout.h"
#include "misc/Types.h"
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
        if (reflected_parameter->second.GetSpirvReflection().TryGetConstant() == nullptr) {
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
        const auto* shader_constant = parameter.GetSpirvReflection().TryGetConstant();
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

void ValidateBindlessIndexAddress(
    const PipelineArgumentMetadata& cpp_metadata,
    const ShaderParametersInfoMap&  shader_parameters,
    uint32                          max_constant_byte_size
) {
    for (const auto& [name, parameter] : shader_parameters.reflect_map) {
        const auto* bindless = parameter.GetSpirvReflection().TryGetBindlessArray();
        if (!bindless || !bindless->index_table_address ||
            !bindless->index_table_address->custom_flag.active) {
            continue;
        }
        const auto&  address         = *bindless->index_table_address;
        const uint64 expected_offset = (uint64(cpp_metadata.GetConstantByteSize()) + 7) & ~uint64(7);
        if (address.offset != expected_offset || address.size != sizeof(uint64)) {
            throw std::invalid_argument("Bindless index-table address disagrees with the constant layout");
        }
        uint32 bindless_arguments = 0;
        for (const auto& argument : cpp_metadata.GetArguments()) {
            bindless_arguments += argument.cpp_info.type == SDA_BindlessArray;
        }
        if (bindless_arguments != 1) {
            throw std::invalid_argument("An active bindless index address requires exactly one bindless argument");
        }
        if (expected_offset + sizeof(uint64) > max_constant_byte_size) {
            throw std::invalid_argument("User constants and bindless address exceed the device limit");
        }
    }
}

} // namespace

uint32 ValidatePipelineConstants(const PipelineShaderInfo& pipeline_shader_info, uint32 max_constant_byte_size) {
    const auto& cpp_metadata            = pipeline_shader_info.argument_metadata;
    bool        pipeline_uses_constants = false;
    for (const auto& shader : pipeline_shader_info.shaders) {
        if (shader.shader_param_map == nullptr) {
            continue;
        }
        const auto& shader_parameters = *shader.shader_param_map;
        ValidateBindlessIndexAddress(cpp_metadata, shader_parameters, max_constant_byte_size);
        ValidateConstantBindingTypes(cpp_metadata, shader_parameters);
        if (ValidateShaderConstantUsage(cpp_metadata, shader_parameters, shader.name)) {
            pipeline_uses_constants = true;
        }
    }

    const uint32 upload_byte_size = pipeline_uses_constants ? cpp_metadata.GetConstantByteSize() : 0;
    if (upload_byte_size > max_constant_byte_size) {
        throw std::invalid_argument(
            "Pipeline constants exceed the device limit: " + std::to_string(upload_byte_size) + " > " +
            std::to_string(max_constant_byte_size)
        );
    }
    return upload_byte_size;
}

} // namespace Moer::Render
