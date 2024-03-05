#include "shader/ShaderCommon.h"
#include "misc/Hash.h"
#include "misc/MacroUtils.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResource.h"
#include <algorithm>
#include <stdexcept>

#pragma region shaderParameters metadata

// const char* g_global_shader_resource_root_dir   = MACRO_STR(GLOBAL_SHADER_RESOURCE_ROOT);
// const char* g_global_shader_resource_output_dir = MACRO_STR(GLOBAL_SHADER_RESOURCE_OUTPUT);

ShaderParametersMetadata::ShaderParametersMetadata(
    EShaderParameterUseCase    _use_case,
    std::string_view           _struct_name,
    uint32_t                   _size,
    const Moer::Array<Member>& _members,
    bool                       _b_force_complete_initialization)
    : use_case(_use_case),
      struct_name(_struct_name),
      size(_size),
      members(_members) {
}

ShaderParametersMetadata::~ShaderParametersMetadata() {
    if (IsLayoutInitialized()) {
        //todo: release layout registration
        layout->~RHIShaderRootParameterLayout();
    }
};

std::string_view ShaderParametersMetadata::GetMemberNameByOffset(uint16_t _member_offset) const {
    const auto& members = GetMembers();

    const auto& iter = std::lower_bound(members.begin(), members.end(), _member_offset, std::less<ShaderParametersMetadata::Member>());

    if (iter != members.end()) {
        return iter->GetName();
    }
    return "";
}
// void ShaderParametersMetadata::InitializeLayout(RHIGlobalBufferLayoutInitializer* _out_layout_initializer) {
//     assert(!IsLayoutInitialized() && "Layout Already Initialized");

//     RHIGlobalBufferLayoutInitializer  temp_initializer(struct_name);
//     RHIGlobalBufferLayoutInitializer& initializer = _out_layout_initializer == nullptr ? temp_initializer : *_out_layout_initializer;

//     initializer.constant_buffer_size = size;

//     initializer.binding_flags = binding_flags;
// }

void ShaderParametersMetadata::InitializeLayout() {
    assert(!IsLayoutInitialized() && "Layout Already Initialized");

    const auto&                   members     = GetMembers();
    RHIShaderRootParameterLayout* temp_layout = new RHIShaderRootParameterLayout();
    RHIShaderRootParameterLayout& layout      = *temp_layout;

    auto is_resource = [](EShaderBindingBaseType _base_type) {
        switch (_base_type) {

            case SBT_INVALID:
                throw std::runtime_error("shader parameter type not supported");
            case SBT_BOOL:
            case SBT_INT32:
            case SBT_UINT32:
            case SBT_FLOAT32:
                return false;
            case SBT_CBV:
            case SBT_SRV:
            case SBT_UAV:
            case SBT_SAMPLER:
                return true;
            default: break;
        }
        return false;
    };

    for (const auto& member : members) {
        EShaderBindingBaseType base_type = member.GetBaseType();
        if (is_resource(base_type)) {

            layout.resource_parameters.emplace_back(
                RHIResourceParameterLayout(member.GetOffset(),
                                           member.GetStride(),

                                           base_type));
        }
    }
    this->layout = &layout;
}
#pragma endregion

/**
 * @brief Registrate ShaderMetaType on initalization
 * 
 */
void ShaderMetaType::OnRegistration() {
    GetNameToTypeMap().insert({type_name_hash, this});
    ShaderCompileRegistration::RegistrateCompileWorkIfNeed(*this);
    //worker
}
ShaderMetaType::ShaderMetaType(
    std::string_view                            _type_name,
    std::string_view                            _file_name,
    std::string_view                            _entry_point,
    EShaderType                                 _shader_type,
    uint32_t                                    _type_size,
    const ShaderParametersMetadata*             _parameter_data,
    uint32_t                                    _total_mutation_count,
    ShaderMetaType::ConstructShaderInstanceProc _shader_type_constructor,
    ShaderMetaType::ShouldCompileMutationProc   _should_compile_mutation,
    SetCompileEnvironmentProc                   _set_compile_environment)
    : type_name(_type_name),
      file_name(_file_name),
      entry_point(_entry_point),
      shader_type(_shader_type),
      parameter_meta_data(_parameter_data),
      total_mutation_count(_total_mutation_count),
      construct_shader_instance(_shader_type_constructor),
      should_compile_mutation(_should_compile_mutation),
      set_compile_environment(_set_compile_environment),
      type_name_hash(GetHash(_type_name)) {

    OnRegistration();
};
ShaderMetaType::~ShaderMetaType() {
    GetNameToTypeMap().erase(type_name_hash);
}

Moer::UnorderedMap<ShaderTypeKey, ShaderMetaType*>& ShaderMetaType::GetNameToTypeMap() {
    // static Moer::UnorderedMap<std::string_view, ShaderMetaType*> name_to_shader_meta_type;
    static Moer::UnorderedMap<ShaderTypeKey, ShaderMetaType*> type_to_shader_meta_type;
    return type_to_shader_meta_type;
}

ShaderMetaType* ShaderMetaType::GetShaderMetaType(uint32_t _type_name_hash) {
    auto& name_to_type_map = GetNameToTypeMap();
    auto  iter             = name_to_type_map.find(_type_name_hash);
    if (iter != name_to_type_map.end() && name_to_type_map.count(_type_name_hash) > 0) {
        return iter->second;
    }
    return nullptr;
}

ShaderMetaType* ShaderMetaType::GetShaderMetaType(std::string_view _type_name) {
    return GetShaderMetaType(GetHash(_type_name));
}

void ShaderMetaType::RegistrateShaderMetaType(ShaderMetaType* type) {
    GetNameToTypeMap().insert({type->type_name_hash, type});
}

ShaderTypeRegistration::ShaderTypeRegistration(std::function<ShaderMetaType&()> _callback) {
    GetRegistrations().push_back(_callback);
}

Moer::Array<std::function<ShaderMetaType&()>>& ShaderTypeRegistration::GetRegistrations() {
    static Moer::Array<std::function<ShaderMetaType&()>> registrations;
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
    Moer::Array<std::function<ShaderMetaType&()>> temp;
    temp.swap(GetRegistrations());
}

Moer::Array<ShaderCompileJobInput> g_compile_job_inputs;

void ShaderCompileRegistration::RegistrateCompileWorkIfNeed(const ShaderMetaType& _shader_type) {
    ShaderTargetInfo target_info;
    std::string      entry_point;
    std::string      relative_source_file_path;
    std::string      shader_name;

    const ShaderParametersMetadata* param_meta_data;
    assert(g_rhi);

    g_compile_job_inputs.emplace_back(
        ShaderTargetInfo{_shader_type.GetShaderType(), GetShaderPlatformByRHIType(g_rhi->GetType())},
        _shader_type.GetEntryPoint(),
        _shader_type.GetFileName(),
        _shader_type.GetName(),
        _shader_type.GetNameHash(),
        _shader_type.GetTotalMutationCount(),
        _shader_type.GetParameterMetaData());
}

Moer::Array<ShaderCompileJobInput>& ShaderCompileRegistration::RetrieveShaderCompileWorks() {
    return g_compile_job_inputs;
}

ShaderCompiledInitializer::ShaderCompiledInitializer(
    const ShaderMetaType*       _shader_type,
    const ShaderCompilerOutput& _compiled_output
    //        const FVertexFactoryType* InVertexFactoryType
    )
    : type_info(_shader_type),
      compiled_code(_compiled_output.shader_code),
      target_info(_compiled_output.target_info),
      parameter_map(_compiled_output.parameter_map),
      compiled_hash(_compiled_output.compiled_hash),
      code_size(_compiled_output.shader_code.size()){};