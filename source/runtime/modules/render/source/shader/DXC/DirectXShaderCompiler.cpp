#include "DirectXShaderCompiler.h"
#include "misc/Hash.h"

#include "wsl/wrladapter.h"
#include "dxguids/dxguids.h"

#include "platform/Platform.h"
#include <cstddef>
#include <filesystem>
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include "DXCUtils.h"
#include "spirv_reflect.h"
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdint.h>
#include <string>

#include "dxc/dxcapi.h"
#include "shader/ShaderCommon.h"

std::function<void(const ShaderCompilerInput& input, ShaderCompilerOutput& output)> DXCompiler::s_compiler_func_table[EShaderPlatform::SP_Num]{};

using Microsoft::WRL::ComPtr;
static ComPtr<IDxcCompiler3> compiler  = nullptr;
static ComPtr<IDxcValidator> validator = nullptr;
static ComPtr<IDxcLibrary>   library   = nullptr;
static ComPtr<IDxcUtils>     utils     = nullptr;

DXCompiler& DXCompiler::GetInstance() {
    static DXCompiler s_compiler;
    return s_compiler;
}
DXCompiler::~DXCompiler() {
    utils->Release();
    library->Release();
    compiler->Release();
}

DXCompiler::DXCompiler() {
    // Initialize DXC library
    HRESULT hres = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hres)) {
        LOG_ERROR("dxcompiler library load fail.");
        return;
    }

    hres = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hres)) {
        LOG_ERROR("Could not init DXC Compiler");
        return;
    }

    // Initialize DXC utility
    hres = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hres)) {
        LOG_ERROR("Could not init DXC Utiliy");
        return;
    }
    utils->AddRef();
    library->AddRef();
    compiler->AddRef();

    s_compiler_func_table[0] = std::bind(&DXCompiler::CompileD3D12, this, std::placeholders::_1, std::placeholders::_2);
    s_compiler_func_table[1] = std::bind(&DXCompiler::CompileVulkan, this, std::placeholders::_1, std::placeholders::_2);
}

void DXCompiler::Compile(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {

    s_compiler_func_table[_input.target_info.shader_platform](_input, _output);
    // CompileVulkan(_input, _output);
}

void DXCompiler::CompileD3D12(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
}

void DXCompiler::CompileVulkan(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
    auto on_fail = [&](const char* messsage) {
        _output.errors.push_back(messsage);
        _output.b_succeeded = false;
        _output.target_info = _input.target_info;
    };

    const char*        file_name_c = _input.relative_source_file_path.c_str();
    const std::wstring entry_name  = std::wstring(_input.entry_point.begin(), _input.entry_point.end());
    HRESULT            hres;

    std::filesystem::path file_path    = g_global_shader_resource_root_dir;
    std::filesystem::path output_path  = g_global_shader_resource_output_dir;
    std::string           platform_str = ToString(_input.target_info.shader_platform);
    if (platform_str.empty()) {
        on_fail("platform not supported");
        return;
    }
    auto platform_output_dir = output_path / platform_str;
    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    if (!std::filesystem::exists(file_path)) {
        on_fail(std::format("Could not load shader file: {}", file_path.generic_string()).c_str());

        return;
    }
    // Load the HLSL text shader from disk
    uint32_t                 code_page = DXC_CP_ACP;
    ComPtr<IDxcBlobEncoding> source_blob;

    //Read source file
    std::ifstream     file_stream(file_path);
    std::stringstream file_source_stream;
    file_source_stream << file_stream.rdbuf();
    if (file_source_stream.str().empty()) {
        on_fail(std::format("shader file {} is empty", file_path.generic_string()).c_str());
        return;
    }
    hres = utils->CreateBlob(file_source_stream.str().data(), file_source_stream.str().size(), CP_UTF8, source_blob.GetAddressOf());

    if (FAILED(hres)) {
        on_fail("Could not load shader file");
        return;
    }

    // Select target profile based on shader file extension
    auto target_profile = GetPlatform(_input.target_info.shader_type, _input.target_info.shader_platform);

    size_t idx = file_name.rfind('.');

    if (idx != std::string::npos) {
        // todo: check file idx to match target
    }

    // Configure the compiler arguments for compiling the HLSL shader to SPIR-V
    std::vector<LPCWSTR> arguments = {
        // (Optional) name of the shader file to be displayed e.g. in an error message
        file_name.c_str(),
        // Shader main entry point
        L"-E",
        entry_name.c_str(),
        // Shader target profile
        L"-T",
        target_profile.c_str(),
        // Compile to SPIRV
        L"-spirv",
        // L"-fspv-reflect",
        DXC_ARG_ALL_RESOURCES_BOUND,
        DXC_ARG_DEBUG,
        DXC_ARG_SKIP_OPTIMIZATIONS};

    // Compile shader
    DxcBuffer buffer{};
    buffer.Encoding = DXC_CP_ACP;
    buffer.Ptr      = source_blob->GetBufferPointer();
    buffer.Size     = source_blob->GetBufferSize();

    ComPtr<IDxcResult> result{nullptr};
    hres = compiler->Compile(
        &buffer,
        arguments.data(),
        (uint32_t)arguments.size(),
        nullptr,
        IID_PPV_ARGS(&result));

    if (SUCCEEDED(hres)) {
        result->GetStatus(&hres);
    }

    // Output error if compilation failed
    if (FAILED(hres) && (result)) {
        ComPtr<IDxcBlobEncoding> error_blob;
        hres = result->GetErrorBuffer(&error_blob);
        std::stringstream error_stream;
        if (SUCCEEDED(hres) && error_blob) {

            error_stream << "Shader compilation failed :\n"
                         << (const char*)error_blob->GetBufferPointer();
            on_fail("Shader compilation failed");
            _output.errors.push_back((const char*)error_blob->GetBufferPointer());

            LOG_INFO(error_stream.str());
            return;
        }
    }

    // Get compilation result
    IDxcBlob* code;
    result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();
    _output.b_succeeded = true;

    _output.shader_code.resize(size);

    _output.compiled_hash.FromData(data, size);
    LOG_INFO("file {} compiled hash: {}", file_path.string(), _output.compiled_hash.ToString());
    memcpy(&_output.shader_code[0], data, size);

    SpvReflectShaderModule module;
    SpvReflectResult       ref_result = spvReflectCreateShaderModule(size, data, &module);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    // Enumerate and extract shader's input variables
    uint32_t var_count = 0;
    ref_result         = spvReflectEnumerateInputVariables(&module, &var_count, NULL);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);
    std::vector<SpvReflectInterfaceVariable*> input_vars(var_count);
    ref_result = spvReflectEnumerateInputVariables(&module, &var_count, input_vars.data());
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    // Output variables, descriptor bindings, descriptor sets, and push constants
    // can be enumerated and extracted using a similar mechanism.
    // module.
    // Destroy the reflection data when no longer required.
    //generate pipeline layout

    std::unordered_map<std::string, ParameterInfo> param_map;
    const ShaderParametersMetadata*                meta_data = _input.param_meta_data;
    for (uint32_t binding_index = 0; binding_index < module.descriptor_binding_count; ++binding_index) {
        auto& binding = module.descriptor_bindings[binding_index];

        auto& param = param_map[binding.name];
        param.slot  = binding.binding;
        param.space = binding.set;
        param.type  = ToShaderParameterType(binding.resource_type);
        param.stage = ToPipelineStageFlag(module.shader_stage);
        param.num   = binding.count;
    }

    for (uint32_t input_index = 0; input_index < module.input_variable_count && _input.target_info.shader_type == EShaderType::ST_VERTEX; input_index++) {

        auto& vertex_input = module.input_variables[input_index];
    }

    const auto&              members = meta_data->GetMembers();
    std::vector<std::string> not_reflected_members;
    for (const ShaderParametersMetadata::Member& member : members) {
        EShaderBindingBaseType base_type = member.GetBaseType();
        std::string            name      = member.GetName();

        auto entry = param_map.find(name);
        auto end   = param_map.end();
        auto count = param_map.count(name);
        if (count <= 0) {
            if (BindingTypeToParameterType(base_type) == EShaderParameterType::CONSTANT_STRUCT) {
                //is root constant
                param_map[name].type = EShaderParameterType::CONSTANT_STRUCT;
            }

            not_reflected_members.push_back(std::format("param {} not found in shader reflection data", member.GetName()));
            continue;
        }
        const auto& param = entry->second;
        //check type
        if (BindingTypeToParameterType(base_type) != param.type) {
            if (BindingTypeToParameterType(base_type) == EShaderParameterType::CONSTANT_STRUCT && param.type == EShaderParameterType::CBV) {
                //is root constant
                param_map[name].type = EShaderParameterType::CONSTANT_STRUCT;
            } else {
                not_reflected_members.push_back(std::format("param {} format mismatch! param format: {}, shader format {}", member.GetName(), ToString(base_type), ToString(param.type)));
                continue;
            }
        }
        LOG_INFO("param {}: {{ slot:{}, set:{}, array_num:{} }}", member.GetName(), param.slot, param.space, param.num);
    }

    for (const auto& msg : not_reflected_members) {
        LOG_ERROR(msg);
    }
    _output.parameter_map.param_map.swap(param_map);
    _output.target_info = _input.target_info;

    spvReflectDestroyShaderModule(&module);
}

bool DXCompiler::IsSupportTarget(const ShaderTargetInfo& _target_info) {
    bool b_support_platform = false;
    switch (_target_info.shader_platform) {

        case SP_WIN_D3D_SM6:
        case SP_VULKAN_SM6:
            b_support_platform = true;
            break;
        case SP_Num:
        case SP_NumBits: break;
    }
    bool b_support_shader_type = false;

    switch (_target_info.shader_type) {

        case ST_VERTEX:
        case ST_GEOMETRY:
        case ST_FRAGMENT:
        case ST_COMPUTE:

        case ST_MESH:
        case ST_AMPLIFICATION:

        case ST_RAY_GEN:
        case ST_RAY_MISS:
        case ST_RAY_HIT:
        case ST_RAY_CALLABLE:
            b_support_shader_type = true;
            break;
        case ST_Num: break;
    }
    return b_support_platform && b_support_shader_type;
}
