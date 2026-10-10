#include "rhi/ShaderConstantLayout.h"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Moer::Render {

uint ValidatePipelineConstants(const PipelineShaderInfo& shader_info, uint max_byte_size) {
    const auto& layout = shader_info.constant_layout;
    if (shader_info.layout_hash.size() != shader_info.arg_cpp_info.size()) {
        throw std::invalid_argument("Shader argument names and types have different lengths");
    }
    std::unordered_set<std::string_view> names;
    for (const auto name : shader_info.layout_hash) {
        if (!names.insert(name).second) throw std::invalid_argument("Duplicate shader argument: " + std::string(name));
    }
    uint64 previous_end = 0;
    std::unordered_set<uint> constant_indices;
    for (const auto& argument : layout.arguments) {
        if (argument.argument_index >= shader_info.arg_cpp_info.size() ||
            shader_info.arg_cpp_info[argument.argument_index].type != SDA_Constant ||
            !constant_indices.insert(argument.argument_index).second ||
            argument.byte_offset % 4 != 0 || argument.byte_size % 4 != 0 ||
            argument.byte_offset < previous_end ||
            uint64(argument.byte_offset) + argument.byte_size > layout.byte_size) {
            throw std::invalid_argument("Invalid pipeline constant argument layout");
        }
        previous_end = uint64(argument.byte_offset) + argument.byte_size;
    }
    for (uint index = 0; index < shader_info.arg_cpp_info.size(); ++index) {
        if (shader_info.arg_cpp_info[index].type == SDA_Constant && !constant_indices.contains(index)) {
            throw std::invalid_argument("Missing constant layout for argument: " + std::string(shader_info.layout_hash[index]));
        }
    }
    if (layout.byte_size % 4 != 0 || (layout.arguments.empty() && layout.byte_size != 0)) {
        throw std::invalid_argument("Invalid pipeline constant data size");
    }

    bool active = false;
    for (const auto& shader : shader_info.shaders) {
        if (shader.shader_param_map == nullptr) continue;
        for (const auto& argument : layout.arguments) {
            const auto name = shader_info.layout_hash[argument.argument_index];
            const auto found = shader.shader_param_map->reflect_map.find(std::string(name));
            if (found != shader.shader_param_map->reflect_map.end() &&
                !std::holds_alternative<ReflectParamInfo::Constant>(found->second.spirv.resources.data)) {
                throw std::invalid_argument("Shader constant argument has a resource binding: " + std::string(name));
            }
        }
        for (const auto& [name, parameter] : shader.shader_param_map->reflect_map) {
            if (name == ReflectParamInfo::bdls_name) continue;
            const auto* constant = std::get_if<ReflectParamInfo::Constant>(&parameter.spirv.resources.data);
            if (constant == nullptr || !constant->custom_flag.active) continue;
            active = true;
            const ShaderConstantArgumentLayout* expected = nullptr;
            for (const auto& argument : layout.arguments) {
                if (shader_info.layout_hash[argument.argument_index] == name) {
                    expected = &argument;
                    break;
                }
            }
            if (expected == nullptr) {
                throw std::invalid_argument("Undeclared active constant '" + std::string(name) +
                                            "' in shader '" + std::string(shader.name) + "'");
            }
            if (constant->offset != expected->byte_offset ||
                constant->size == 0 || constant->size > expected->byte_size) {
                throw std::invalid_argument("Shader constant layout mismatch for '" + std::string(name) +
                    "' in shader '" + std::string(shader.name) + "': expected offset=" +
                    std::to_string(expected->byte_offset) + ", available size=" +
                    std::to_string(expected->byte_size) + ", reflected offset=" +
                    std::to_string(constant->offset) + ", size=" + std::to_string(constant->size));
            }
        }
    }
    if (active && (layout.byte_size == 0 || layout.byte_size > max_byte_size)) {
        throw std::invalid_argument("Pipeline constants exceed the device limit: " +
                                    std::to_string(layout.byte_size) + " > " + std::to_string(max_byte_size));
    }
    return active ? layout.byte_size : 0;
}

} // namespace Moer::Render
