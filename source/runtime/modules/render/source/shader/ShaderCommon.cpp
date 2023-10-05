#include "shader/ShaderCommon.h"
#include "misc/Hash.h"
#include <vector>

#pragma region shaderParameters metadata

ShaderParametersMetadata::ShaderParametersMetadata(
    EShaderParameterUseCase           _use_case,
    EGlobalBufferBindingFlags         _binding_flags,
    const char*                       _struct_name,
    const char*                       _shader_variable_name,
    uint32_t                          _size,
    const std::vector<Member>&        _members,
    bool                              _b_force_complete_initialization,
    RHIGlobalBufferLayoutInitializer* _out_layout_initializer)
    : use_case(_use_case),
      binding_flags(_binding_flags),
      struct_name(_struct_name),
      shader_variable_name(_shader_variable_name),
      size(_size),
      members(_members) {
}

ShaderParametersMetadata::~ShaderParametersMetadata(){
    if(IsLayoutInitialized()){
        //todo: release layout registration
    }
};

void ShaderParametersMetadata::GetNestedStructs(std::vector<const ShaderParametersMetadata*>& _out_nested_structs) const {
    for (const auto& member : members) {
        const ShaderParametersMetadata* meta_data = member.GetStructMetadata();
        if(meta_data){
            _out_nested_structs.push_back(meta_data);
            meta_data->GetNestedStructs(_out_nested_structs);
        }
    }
}
void ShaderParametersMetadata::FindMemberFromOffset(uint16_t MemberOffset, const ShaderParametersMetadata** OutContainingStruct, const ShaderParametersMetadata::Member** OutMember, int32_t* ArrayElementId, std::string* NamePrefix) const {

}
std::string ShaderParametersMetadata::GetFullMemberCodeName(uint16_t MemberOffset) const {
    return "";
}
void ShaderParametersMetadata::InitializeLayout(RHIGlobalBufferLayoutInitializer* _out_layout_initializer) {
    assert(!IsLayoutInitialized() && "Layout Already Initialized");

    RHIGlobalBufferLayoutInitializer temp_initializer(struct_name);
    RHIGlobalBufferLayoutInitializer& initializer = _out_layout_initializer == nullptr ? temp_initializer : *_out_layout_initializer;

    initializer.constant_buffer_size = size;
    initializer.static_slot = slot;
    initializer.binding_flags = binding_flags;

}
#pragma endregion

void ShaderTypeInfo::OnRegistration() {
    //todo: registration
}

void ShaderTypeRegistration::CollectRegistration(std::function<ShaderTypeInfo*()> _registration_func) {
    registration_callbacks.push_back(_registration_func);
}

void ShaderTypeRegistration::SubmitRegistrations() {
    for (const auto& registration_func : registration_callbacks) {
        ShaderTypeInfo* info = registration_func();
        //todo: later process
    }
    std::vector<std::function<ShaderTypeInfo*()>> temp;
    temp.swap(registration_callbacks);
}

void ShaderCompilerOutput::GenerateCompiledHash() {
    Hash64City& hash = compiled_hash;
    hash.Update(shader_code.data(), shader_code.size());

    auto param_map = parameter_map.GetShaderParameterMap();
    for (const auto& param : param_map) {
        const auto& name        = param.first;
        const auto& param_value = param.second;
        hash.Update(name.data(), name.length());
        hash.Update((const char*)(&param_value.type), sizeof(EShaderParameterType));
        hash.Update((const char*)(&param_value.buffer_index), sizeof(uint16_t));
        hash.Update((const char*)(&param_value.slot), sizeof(uint16_t));
        hash.Update((const char*)(&param_value.size), sizeof(uint16_t));
    }
}
