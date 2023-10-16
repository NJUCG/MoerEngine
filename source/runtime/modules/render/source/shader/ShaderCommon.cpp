#include "shader/ShaderCommon.h"
#include "misc/Hash.h"
#include "misc/MacroUtils.h"
#include <vector>

#pragma region shaderParameters metadata

const char* g_global_shader_resource_root_dir   = MACRO_STR(GLOBAL_SHADER_RESOURCE_ROOT);
const char* g_global_shader_resource_output_dir = MACRO_STR(GLOBAL_SHADER_RESOURCE_OUTPUT);

ShaderParametersMetadata::ShaderParametersMetadata(
    EShaderParameterUseCase           _use_case,
    EGlobalBufferBindingFlags         _binding_flags,
    const char*                       _struct_name,
    uint32_t                          _size,
    const std::vector<Member>&        _members,
    bool                              _b_force_complete_initialization,
    RHIGlobalBufferLayoutInitializer* _out_layout_initializer)
    : use_case(_use_case),
      binding_flags(_binding_flags),
      struct_name(_struct_name),
      size(_size),
      members(_members) {
}

ShaderParametersMetadata::~ShaderParametersMetadata() {
    if (IsLayoutInitialized()) {
        //todo: release layout registration
    }
};

void ShaderParametersMetadata::GetNestedStructs(std::vector<const ShaderParametersMetadata*>& _out_nested_structs) const {
    for (const auto& member : members) {
        const ShaderParametersMetadata* meta_data = member.GetStructMetadata();
        if (meta_data) {
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

    RHIGlobalBufferLayoutInitializer  temp_initializer(struct_name);
    RHIGlobalBufferLayoutInitializer& initializer = _out_layout_initializer == nullptr ? temp_initializer : *_out_layout_initializer;

    initializer.constant_buffer_size = size;

    initializer.binding_flags = binding_flags;
}
#pragma endregion

/**
 * @brief Registrate ShaderMetaType on initalization
 * 
 */
void ShaderMetaType::OnRegistration() {
    GetNameToTypeMap().insert({type_name, this});
    ShaderCompileRegistration::RegistrateCompileWorkIfNeed(*this);
    //worker
}
ShaderMetaType::ShaderMetaType(
    const char*                     _type_name,
    const char*                     _file_name,
    const char*                     _entry_point,
    EShaderType                     _shader_type,
    uint32_t                        _type_size,
    const ShaderParametersMetadata* _parameter_data)
    : type_name(_type_name),
      hash_type_name(type_name),
      file_name(_file_name),
      hash_file_name(file_name),
      entry_point(_entry_point),
      shader_type(_shader_type),
      parameter_meta_data(_parameter_data) {

    OnRegistration();
};
ShaderMetaType::~ShaderMetaType() {
    GetNameToTypeMap().erase(type_name);
}

std::unordered_map<std::string, ShaderMetaType*>& ShaderMetaType::GetNameToTypeMap() {
    static std::unordered_map<std::string, ShaderMetaType*> name_to_shader_meta_type;
    return name_to_shader_meta_type;
}

ShaderTypeRegistration::ShaderTypeRegistration(std::function<ShaderMetaType&()> _callback) {
    GetRegistrations().push_back(_callback);
}

std::vector<std::function<ShaderMetaType&()>>& ShaderTypeRegistration::GetRegistrations() {
    static std::vector<std::function<ShaderMetaType&()>> registrations;
    return registrations;
}
void ShaderTypeRegistration::CollectRegistration(std::function<ShaderMetaType&()> _registration_func) {
    GetRegistrations().push_back(_registration_func);
}

void ShaderTypeRegistration::SubmitRegistrations() {
    for (const auto& registration_func : GetRegistrations()) {
        ShaderMetaType& info = registration_func();
        //todo: later process
    }
    std::vector<std::function<ShaderMetaType&()>> temp;
    temp.swap(GetRegistrations());
}

std::vector<ShaderCompilerInput> g_compiled_inputs;

void ShaderCompileRegistration::RegistrateCompileWorkIfNeed(const ShaderMetaType& _shader_type) {
    ShaderTargetInfo target_info;
    std::string      entry_point;
    std::string      relative_source_file_path;
    std::string      shader_name;

    const ShaderParametersMetadata* param_meta_data;
    assert(g_rhi);

    g_compiled_inputs.emplace_back(
        ShaderTargetInfo{_shader_type.GetShaderType(), GetShaderPlatformByRHIType(g_rhi->GetType())}, _shader_type.GetEntryPoint(), _shader_type.GetFileName(), _shader_type.GetName(), _shader_type.GetParameterMetaData());
}

std::vector<ShaderCompilerInput>& ShaderCompileRegistration::RetrieveShaderCompileWorks() {
    return g_compiled_inputs;
}
