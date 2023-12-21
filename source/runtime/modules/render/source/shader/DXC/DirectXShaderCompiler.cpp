#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"

#include "rhi/RHI.h"
#include "wsl/wrladapter.h"
#include "dxguids/dxguids.h"

#include "platform/Platform.h"
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
#include <string_view>

#include "dxc/dxcapi.h"
#include "shader/ShaderCommon.h"
#include "DirectXShaderReflectorVulkan.h"

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
    delete reflector;
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

    if (g_rhi->GetType() == ERHIType::Vulkan)
        reflector = new DirectXShaderReflectorVulkan();
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

    std::filesystem::path file_path    = Moer::ConfigManager::GetInstance().GetEngineShaderPath();
    std::filesystem::path output_path  = file_path;
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
    Moer::Array<LPCWSTR> arguments = {
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
        // L"-fvk-invert-y",
        // L"-fvk-use-dx-position-w",
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
    // const auto& file_name_str   = file_path.generic_string();
    // const auto& output_hash_str = _output.compiled_hash.ToString();

    // std::string new_file_name_str   = file_name_str;
    // std::string new_output_hash_str = output_hash_str;

    // std::string_view file_name_view(file_name_str);
    // std::string_view output_hash_view(output_hash_str);
    // LOG_INFO("file {} compiled hash: {}", file_name_view, output_hash_view);
    memcpy(&_output.shader_code[0], data, size);

    // int32_t* test_mi_override = new int32_t[10];
    // delete[] test_mi_override;

    // test_mi_override = (int32_t*)malloc(10 * sizeof(int32_t));
    // free(test_mi_override);

    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
    reflector->ReflectShader(result.Get(), _input.param_meta_data, param_map);
    _output.parameter_map.param_map.swap(param_map);
    _output.target_info = _input.target_info;
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
        default: break;
    }
    return b_support_platform && b_support_shader_type;
}
