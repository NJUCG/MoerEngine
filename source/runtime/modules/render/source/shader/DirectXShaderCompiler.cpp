#include "DirectXShaderCompiler.h"
#include "misc/Hash.h"
#include "platform/Platform.h"
#include <cstddef>
#include <filesystem>
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include "spirv_reflect.h"
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vcruntime_string.h>

#include "wsl/winadapter.h"

#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#elif PLATFORM_LINUX
typdef long HRESULT;
typdef wchar_t WCHAR
#endif
// #include "Windows.h"
#include <winnt.h>
#include <wrl/client.h>
#endif
#include "dxc/dxcapi.h"
#include "shader/ShaderCommon.h"
EShaderParameterType ToShaderParameterType(SpvReflectResourceType _type);

ERHIPipelineStageFlags ToPipelineStageFlag(SpvReflectShaderStageFlagBits _stage);

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type);

std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform);

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

    // s_compiler_func_table[_input.target_info.shader_platform](_input, _output);
    CompileVulkan(_input, _output);
}

void DXCompiler::CompileD3D12(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
}

void DXCompiler::CompileVulkan(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
    auto OnFail = [&](const char* messsage) {
        _output.errors.push_back(messsage);
        _output.b_succeeded = false;
        _output.target_info = _input.target_info;
    };

    const char*        file_name_c = _input.relative_source_file_path.c_str();
    const std::wstring entry_name  = std::wstring(_input.entry_point.begin(), _input.entry_point.end());
    HRESULT            hres;

    std::filesystem::path file_path = g_global_shader_resource_root_dir;
    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    if (!std::filesystem::exists(file_path)) {
        OnFail("Could not load shader file");
    }

    //ReadFile

    // Initialize DXC compiler

    // Load the HLSL text shader from disk
    uint32_t                 code_page = DXC_CP_ACP;
    ComPtr<IDxcBlobEncoding> source_blob;

    //read source file
    std::ifstream     file_stream(file_path);
    std::stringstream file_source_stream;
    file_source_stream << file_stream.rdbuf();
    if (file_source_stream.str().empty()) {
        OnFail(std::format("shader file {} is empty", file_path.string()).c_str());
        return;
    }
    hres = utils->CreateBlob(file_source_stream.str().data(), file_source_stream.str().size(), CP_UTF8, source_blob.GetAddressOf());

    if (FAILED(hres)) {
        OnFail("Could not load shader file");
        return;
    }

    // Select target profile based on shader file extension
    auto target_profile = GetPlatform(_input.target_info.shader_type, _input.target_info.shader_platform);

    size_t idx = file_name.rfind('.');

    if (idx != std::string::npos) {
        // Mapping for other file types go here (cs_x_y, lib_x_y, etc.)
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
        L"-fspv-reflect",
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
            OnFail("Shader compilation failed");
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

    const auto&              members = meta_data->GetMembers();
    std::vector<std::string> not_reflected_members;
    for (const ShaderParametersMetadata::Member& member : members) {
        member.GetBaseType();
        std::string name = member.GetName();

        auto entry = param_map.find(name);
        auto end   = param_map.end();
        auto count = param_map.count(name);
        if (count <= 0) {
            not_reflected_members.push_back(std::format("param {} not found in shader reflection data", member.GetName()));
            continue;
        }
        const auto& param = entry->second;
        //check type
        if (auto base_type = member.GetBaseType(); BindingTypeToParameterType(base_type) != param.type) {
            not_reflected_members.push_back(std::format("param {} format mismatch! param format: {}, shader format {}", member.GetName(), ToString(base_type), ToString(param.type)));
            continue;
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

EShaderParameterType ToShaderParameterType(SpvReflectResourceType _type) {
    switch (_type) {

        case SPV_REFLECT_RESOURCE_FLAG_UNDEFINED:
            return EShaderParameterType::Num;
        case SPV_REFLECT_RESOURCE_FLAG_SAMPLER:
            return EShaderParameterType::SAMPLER;
        case SPV_REFLECT_RESOURCE_FLAG_CBV:
            return EShaderParameterType::CBV;
        case SPV_REFLECT_RESOURCE_FLAG_SRV:
            return EShaderParameterType::SRV;
        case SPV_REFLECT_RESOURCE_FLAG_UAV:
            return EShaderParameterType::UAV;
            break;
    }
    return EShaderParameterType::Num;
}

ERHIPipelineStageFlags ToPipelineStageFlag(SpvReflectShaderStageFlagBits _stage) {
    switch (_stage) {

        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
            return ERHIPipelineStageFlags::PS_VERTEX_SHADER;

        case SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT:
            return ERHIPipelineStageFlags::PS_GEOMETRY_SHADER;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
            return ERHIPipelineStageFlags::PS_FRAGMENT_SHADER;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
            return ERHIPipelineStageFlags::PS_COMPUTE_SHADER;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
        case SPV_REFLECT_SHADER_STAGE_INTERSECTION_BIT_KHR:
            return ERHIPipelineStageFlags::PS_RAY_TRACING_SHADER;
        case SPV_REFLECT_SHADER_STAGE_TASK_BIT_NV:
        case SPV_REFLECT_SHADER_STAGE_MESH_BIT_NV:
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        case SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        case SPV_REFLECT_SHADER_STAGE_CALLABLE_BIT_KHR: break;
    }
    return ERHIPipelineStageFlags::PS_NONE;
}

EShaderParameterType BindingTypeToParameterType(EShaderBindingBaseType _type) {
    switch (_type) {

        case SBT_INVALID:
        case SBT_BOOL:
        case SBT_INT32:
        case SBT_UINT32:
        case SBT_FLOAT32:
            return EShaderParameterType::Num;
        case SBT_CBV:
            return EShaderParameterType::CBV;
        case SBT_SRV:
            return EShaderParameterType::SRV;
        case SBT_UAV:
            return EShaderParameterType::UAV;
        case SBT_SAMPLER:
            return EShaderParameterType::SAMPLER;
        case SBT_ATTACHMENT_BINDING_SLOTS:

        defualt:
            break;
    }
    return EShaderParameterType::Num;
}
const WCHAR* GetShaderTypeWChar(EShaderType _type) {
    switch (_type) {

        case ST_VERTEX:
            return L"vs";
        case ST_GEOMETRY:
            return L"gs";
        case ST_FRAGMENT:
            return L"ps";
        case ST_COMPUTE:
            return L"cs";
        case ST_MESH:
            return L"ms";
        case ST_AMPLIFICATION:
            return L"as";
        case ST_RAY_GEN:
            return L"lib";
        case ST_RAY_MISS:
            return L"lib";
        case ST_RAY_HIT:
            return L"lib";
        case ST_RAY_CALLABLE:
            return L"lib";
        case ST_Num: break;
    }
    return L"";
}

const WCHAR* GetShaderModel(EShaderPlatform _type) {
    switch (_type) {

        case SP_WIN_D3D_SM6:
        case SP_VULKAN_SM6:
            return L"6_7";
        case SP_Num:
        case SP_NumBits: break;
    }
    return L"";
}
std::wstring GetPlatform(EShaderType _type, EShaderPlatform _platform) {
    const WCHAR* type     = GetShaderTypeWChar(_type);
    const WCHAR* platform = GetShaderModel(_platform);
    auto         k        = std::format(L"{}_{}", type, platform);
    return k;
}