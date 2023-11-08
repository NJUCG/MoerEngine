
#include "DirectXShaderReflectorVulkan.h"
#include "rhi/RHICommon.h"
#include "spirv_reflect.h"
#include "wsl/wrladapter.h"
#include "dxguids/dxguids.h"
#include "dxc/dxcapi.h"
#include "DXCUtils.h"
#include "log/LogSystem.h"

#include <format>

void DirectXShaderReflectorVulkan::ReflectShader(const void* _compiled_result, const ShaderParametersMetadata* _param_meta_data, std::unordered_map<std::string, ParameterInfo>& _out_parameters) {
    using Microsoft::WRL::ComPtr;
    // Reflect the shader and fill the unordered map with the results
    ComPtr<IDxcResult> result = (IDxcResult*)_compiled_result;
    assert(result != nullptr && "invalid reflect source data.");
    // Get compilation result
    IDxcBlob* code;
    result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();

    SpvReflectShaderModule reflect_module;
    SpvReflectResult       ref_result = spvReflectCreateShaderModule(size, data, &reflect_module);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    // Enumerate and extract shader's input variables
    uint32_t var_count = 0;
    ref_result         = spvReflectEnumerateInputVariables(&reflect_module, &var_count, NULL);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);
    std::vector<SpvReflectInterfaceVariable*> input_vars(var_count);
    ref_result = spvReflectEnumerateInputVariables(&reflect_module, &var_count, input_vars.data());
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    // Output variables, descriptor bindings, descriptor sets, and push constants
    // can be enumerated and extracted using a similar mechanism.
    // module.
    // Destroy the reflection data when no longer required.
    //generate pipeline layout

    std::unordered_map<std::string, ParameterInfo> param_map;
    const ShaderParametersMetadata*                meta_data = _param_meta_data;
    for (uint32_t binding_index = 0; binding_index < reflect_module.descriptor_binding_count; ++binding_index) {
        auto& binding = reflect_module.descriptor_bindings[binding_index];

        auto& param = param_map[binding.name];
        param.slot  = binding.binding;
        param.space = binding.set;
        param.type  = ToShaderParameterType(binding.resource_type);
        param.stage = ToPipelineStageFlag(reflect_module.shader_stage);
        param.num   = binding.count;
    }

    const auto&              members = meta_data->GetMembers();
    std::vector<std::string> not_reflected_members;
    for (const ShaderParametersMetadata::Member& member : members) {
        EShaderBindingBaseType base_type = member.GetBaseType();
        std::string            name      = member.GetName();

        auto entry = param_map.find(name);
        auto end   = param_map.end();
        auto count = param_map.count(name);

        //for vulkan, push constants don't have binding info, so reflect information depends on user defined shader meta data
        if (count <= 0) {
            if (BindingTypeToParameterType(base_type) == EShaderParameterType::CONSTANT_STRUCT) {
                //push constant
                param_map[name].type = EShaderParameterType::CONSTANT_STRUCT;
            } else {
                not_reflected_members.push_back(std::format("param {} not found in shader reflection data", member.GetName()));
            }
            continue;
        }
        const auto& param = entry->second;
        //check type
        if (BindingTypeToParameterType(base_type) != param.type) {
            //type mismatch
            if (base_type == SBT_CONST_STRUCT) {
                LOG_CRITICAL("push constant member define error, should be writen as\n[[vk::push_constant]]\nConstantBuffer<YourConstantStruct> {};", member.GetName());
                continue;
            }
            not_reflected_members.push_back(std::format("param {} format mismatch! param format: {}, shader format {}", member.GetName(), ToString(base_type), ToString(param.type)));
            continue;
        }
        LOG_INFO("param {}: {{ slot:{}, set:{}, array_num:{} }}", member.GetName(), param.slot, param.space, param.num);
    }

    for (const auto& msg : not_reflected_members) {
        LOG_ERROR(msg);
    }
    _out_parameters.swap(param_map);
    spvReflectDestroyShaderModule(&reflect_module);
}
