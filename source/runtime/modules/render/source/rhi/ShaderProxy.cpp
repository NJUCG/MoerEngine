//
// Created by 17152 on 2023/9/26.
//
#include "rhi/ShaderProxy.h"
ShaderParametersMetadata::ShaderParametersMetadata(
    ShaderParametersMetadata::EUseCase _use_case,
    EUniformBufferBindingFlags         _binding_flags,
    const char*                        _layout_name,
    const char*                        _struct_name,
    const char*                        _shader_variable_name,
    const char*                        _static_slot_name,
    const char*                        _file_name,
    const int32_t                      _file_line,
    uint32_t                           _size,
    const std::vector<Member>&         _members,
    bool                               _b_force_complete_initialization,
    RHIUniformBufferMemberInitializer* _out_layout_initializer,
    uint32_t                           _usage_flags)
    :
    use_case(_use_case),
    binding_flags(_binding_flags),
    layout_name(_layout_name),
    struct_name(_struct_name),
    shader_variable_name(_shader_variable_name),
    static_slot_name(_static_slot_name),
    file_name(_file_name),
    file_line(_file_line),
    size(_size),
    members(_members),
    usage_flags(_usage_flags)
    {
    // for global layout meta data registration
}
void ShaderParametersMetadata::FindMemberFromOffset(
    uint16_t MemberOffset,
    const ShaderParametersMetadata** OutContainingStruct,
    const ShaderParametersMetadata::Member** OutMember,
    int32_t* ArrayElementId,
    std::string* NamePrefix) const {


}
ShaderParametersMetadata::~ShaderParametersMetadata() {
    // for global layout meta data deletion
}
void ShaderParametersMetadata::GetNestedStructs(std::vector<const ShaderParametersMetadata*>& _out_nested_structs) const {
    for (const auto & current_member : members) {
        const ShaderParametersMetadata* member_meta_data = current_member.GetStructMetadata();
        if(member_meta_data != nullptr){
            //push current meta data
            _out_nested_structs.push_back(member_meta_data);
            //push nested meta data
            member_meta_data->GetNestedStructs(_out_nested_structs);
        }
    }
}
std::string ShaderParametersMetadata::GetFullMemberCodeName(uint16_t MemberOffset) const {
    return std::string();
}

std::optional<ShaderParameterAllocationInfo> ShaderParameterMap::FindShaderParameterAllocation(const std::string& _param_name) const {
    if(auto temp_param = shader_parameters_map.find(_param_name); temp_param != shader_parameters_map.end()){
        if(temp_param->second.b_bound){

        }
        temp_param->second.b_bound = true;

        return std::optional<ShaderParameterAllocationInfo>(temp_param->second);
    }
    return {};
}
void ShaderParameterMap::AddShaderParameterAllocation(const char* _param_name, uint16_t _buffer_index, uint16_t _base_index, uint16_t _size, EShaderParameterType _type) {
    assert(_type < EShaderParameterType::Num);
    shader_parameters_map.emplace(_param_name, ShaderParameterAllocationInfo(_buffer_index, _base_index, _size, _type));
}
void ShaderParameterMap::RemoveShaderParameterAllocation(const char* _param_name) {
    shader_parameters_map.erase(_param_name);
}
