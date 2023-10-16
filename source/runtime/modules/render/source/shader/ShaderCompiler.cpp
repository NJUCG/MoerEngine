#include "math/Base.h"
#include "math/Matrix.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/vulkan/IVulkanRHI.h"
#include "shader/ShaderParameterMacros.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include <algorithm>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>

#include "platform/Platform.h"

#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include "Windows.h"
#endif

#include "dxc/dxcapi.h"

#include "shader/ShaderCompiler.h"
#include "shader/Shader.h"
#include "shader/ShaderMap.h"
#include "spirv_reflect.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#include "log/LogSystem.h"
#include "shader/ShaderCommon.h"
#include <vector>
#include <d3d12.h>
#include <atlcomcli.h>
#include <winnt.h>
#include <sstream>
#include "rhi/RHI.h"

class TestReflectionShader : public Shader {
    DEFINE_SHADER_TYPE(TestReflectionShader, Global, )
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    //constant
    DEFINE_SHADER_PARAM(Moer::Vector4f, color)
    DEFINE_SHADER_PARAM_SRV(Buffer, bar)
    //Ubo set
    DEFINE_SHADER_PARAM_UAV(RWBuffer, dataLog)

    DEFINE_SHADER_PARAM_SAMPLER_ARRAY(Sampler[2], samp, 2)
    DEFINE_SHADER_PARAM_SAMPLER(Sampler, aniso)
    //srv set
    DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D[5], foo, 5)
    //uav set
    DEFINE_SHADER_PARAM_CBV(ConstantBuffer<UBO>, ubo)

    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestReflectionShader, "TestVert.vert", "main", EShaderType::ST_VERTEX);

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

        case SBT_Num: break;
    }
    return EShaderParameterType::Num;
}

void ShaderCompiler::CompileD3D12(const ShaderCompilerInput& input, ShaderCompilerOutput& output) {
}

void ShaderCompiler::CompileVulkan(const ShaderCompilerInput& input, ShaderCompilerOutput& output) {

    auto OnFail = [&](const char* messsage) {
        output.errors.push_back(messsage);
        output.b_succeeded = false;
        output.target_info = input.target_info;
    };

    const char*        file_name_c = input.relative_source_file_path.c_str();
    const std::wstring entry_name  = std::wstring(input.entry_point.begin(), input.entry_point.end());
    HRESULT            hres;

    std::filesystem::path file_path = g_global_shader_resource_root_dir;
    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    if (!std::filesystem::exists(file_path)) {
        OnFail("Could not load shader file");
    }

    //ReadFile

    // Initialize DXC library
    CComPtr<IDxcLibrary> library;
    hres = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    if (FAILED(hres)) {
        OnFail("Could not init DXC Library");

        return;
    }

    // Initialize DXC compiler
    CComPtr<IDxcCompiler3> compiler;
    hres = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hres)) {
        OnFail("Could not init DXC Compiler");
        return;
    }

    // Initialize DXC utility
    CComPtr<IDxcUtils> utils;
    hres = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hres)) {
        OnFail("Could not init DXC Utiliy");
        return;
    }

    // Load the HLSL text shader from disk
    uint32_t                  code_page = DXC_CP_ACP;
    CComPtr<IDxcBlobEncoding> source_blob;
    hres = utils->LoadFile(file_name.c_str(), &code_page, &source_blob);
    if (FAILED(hres)) {
        OnFail("Could not load shader file");
        return;
    }

    // Select target profile based on shader file extension
    LPCWSTR target_profile{};
    size_t  idx = file_name.rfind('.');
    if (idx != std::string::npos) {
        std::wstring extension = file_name.substr(idx + 1);
        if (extension == L"vert") {
            target_profile = L"vs_6_4";
        }
        if (extension == L"frag") {
            target_profile = L"ps_6_4";
        }
        // Mapping for other file types go here (cs_x_y, lib_x_y, etc.)
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
        target_profile,
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

    CComPtr<IDxcResult> result{nullptr};
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
        CComPtr<IDxcBlobEncoding> error_blob;
        hres = result->GetErrorBuffer(&error_blob);
        std::stringstream error_stream;
        if (SUCCEEDED(hres) && error_blob) {

            error_stream << "Shader compilation failed :\n\n"
                         << (const char*)error_blob->GetBufferPointer();
            OnFail("Shader compilation failed");
            output.errors.push_back((const char*)error_blob->GetBufferPointer());

            LOG_INFO(error_stream.str());
            return;
        }
    }

    // Get compilation result
    IDxcBlob* code;
    result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();
    output.b_succeeded  = true;

    output.shader_code.resize(size);
    memcpy(&output.shader_code[0], data, size);

    SpvReflectShaderModule module;
    SpvReflectResult       ref_result = spvReflectCreateShaderModule(size, data, &module);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    // Enumerate and extract shader's input variables
    uint32_t var_count = 0;
    ref_result         = spvReflectEnumerateInputVariables(&module, &var_count, NULL);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);
    SpvReflectInterfaceVariable** input_vars =
        (SpvReflectInterfaceVariable**)malloc(var_count * sizeof(SpvReflectInterfaceVariable*));
    ref_result = spvReflectEnumerateInputVariables(&module, &var_count, input_vars);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    std::vector<SpvReflectDescriptorBinding> bindings;
    bindings.resize(module.descriptor_binding_count);

    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = module.descriptor_bindings[i];
    }
    // Output variables, descriptor bindings, descriptor sets, and push constants
    // can be enumerated and extracted using a similar mechanism.
    // module.
    // Destroy the reflection data when no longer required.
    //generate pipeline layout

    std::unordered_map<std::string, ParameterInfo> param_map;
    const ShaderParametersMetadata*                meta_data = input.param_meta_data;
    for (uint32_t binding_index = 0; binding_index < module.descriptor_binding_count; ++binding_index) {
        auto& binding = module.descriptor_bindings[binding_index];

        auto& param = param_map[binding.name];
        param.slot  = binding_index;
        param.space = binding.set;
        param.type  = ToShaderParameterType(binding.resource_type);
        param.stage = ToPipelineStageFlag(module.shader_stage);
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
        LOG_INFO("param {}: {{ slot:{}, set:{} }}", member.GetName(), param.slot, param.space);
    }

    for (const auto& msg : not_reflected_members) {
        LOG_ERROR(msg);
    }
    output.parameter_map.param_map.swap(param_map);
    output.target_info = input.target_info;
    spvReflectDestroyShaderModule(&module);
}

std::function<void(const ShaderCompilerInput& input, ShaderCompilerOutput& output)> ShaderCompiler::g_compiler_func_table[EShaderPlatform::SP_Num]{
    CompileD3D12,
    CompileD3D12,
    CompileVulkan,
    CompileVulkan};

/**
 * @brief cross-compile shader
 * 
 * @param input ShaderCompilerInput: contains all information compiler needs
 * @param output ShaderCompilerOutput: output shader code, error messages and param data bindings
 */
void ShaderCompiler::Compile(const ShaderCompilerInput& input, ShaderCompilerOutput& output) {

    g_compiler_func_table[input.target_info.shader_platform](input, output);
}

class FakeRHI : public IVulkanRHI {
public:
    FakeRHI() {
        rhi_type = ERHIType::Vulkan;
    }
};

void ShaderCompiler::ShaderConductorTest() {
    g_rhi = new FakeRHI;
    TestReflectionShader::GetMetaType();

    ShaderTypeRegistration::SubmitRegistrations();

    const auto& works = ShaderCompileRegistration::RetrieveShaderCompileWorks();

    // GraphEventRef collect_job = GraphTask<EmptyGraphTask>::CreateTask().ConstructAndDispatchWhenReady(EThread::EGameThread);

    // static auto library_instance_ =
    //     CComPtr<IDxcLibrary>([&](auto ptr) {
    //         return DxcCreateInstance(CLSID_DxcLibrary,
    //                                  __uuidof(IDxcLibrary),
    //                                  (LPVOID*)ptr);
    //     });

    // static auto compiler_instance_ =
    //     CComPtr<IDxcCompiler>([&](auto ptr) {
    //         return create_proc(CLSID_DxcCompiler,
    //                            __uuidof(IDxcCompiler),
    //                            (LPVOID*)ptr);
    //     });

    // static auto include_handler_ =
    //     CComPtr<IDxcIncludeHandler>([&](auto ptr) {
    //         return library_instance_->CreateIncludeHandler(ptr);
    //     });

    std::for_each(works.begin(), works.end(), [](const ShaderCompilerInput& input) {
        ShaderCompilerOutput output;
        //todo: check file cached
        Compile(input, output);
    });
}