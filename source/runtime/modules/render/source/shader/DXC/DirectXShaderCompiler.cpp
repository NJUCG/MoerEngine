#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"

#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <zpp_bits.h>
#endif

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

std::function<ShaderCompilerOutput*(const ShaderCompilerInput& input)> DXCompiler::s_compiler_func_table[EShaderPlatform::SP_Num]{};

namespace Moer {
    struct ShaderCompiledCacheFile {
        uint64_t hash;
        uint32_t mutation_id;
        uint32_t platform;
        uint32_t shader_type;

        long long last_write_time;

        Moer::Array<uint8_t> code;
        //reflect data
        Moer::UnorderedMap<std::string, ParameterInfo> param_map;

        bool Equals(const ShaderCompilerInput& input) const {
            if (input.target_info.shader_platform != platform) {
                return false;
            }
            if (input.target_info.shader_type != shader_type) {
                return false;
            }
            if (input.mutation_id != mutation_id) {
                return false;
            }
            return true;
        }
        const auto& GetLastWriteTime() const {
            return last_write_time;
        }
    };
    auto serialize(const ShaderCompiledCacheFile& person) -> zpp::bits::members<7>;
};// namespace Moer

using Microsoft::WRL::ComPtr;
static ComPtr<IDxcCompiler3>      compiler        = nullptr;
static ComPtr<IDxcValidator>      validator       = nullptr;
static ComPtr<IDxcLibrary>        library         = nullptr;
static ComPtr<IDxcUtils>          utils           = nullptr;
static ComPtr<IDxcIncludeHandler> include_handler = nullptr;

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
    library->CreateIncludeHandler(&include_handler);

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
    include_handler->AddRef();

    s_compiler_func_table[SP_WIN_D3D_SM6] = std::bind(&DXCompiler::CompileD3D12, this, std::placeholders::_1);
    s_compiler_func_table[SP_VULKAN_SM6]  = std::bind(&DXCompiler::CompileVulkan, this, std::placeholders::_1);

    if (g_rhi->GetType() == ERHIType::Vulkan)
        reflector = new DirectXShaderReflectorVulkan();
}

ShaderCompilerOutput* DXCompiler::Compile(const ShaderCompilerInput& _input) {

    return s_compiler_func_table[_input.target_info.shader_platform](_input);
}

ShaderCompilerOutput* DXCompiler::CompileD3D12(const ShaderCompilerInput& _input) {
    return nullptr;
}

bool LoadCache(long long _last_write_time, const ShaderCompilerInput& input, ShaderCompilerOutput& output) {
    uint32_t          shader_name_hash = GetHash(input.shader_name);
    ShaderResourceKey key{shader_name_hash, input.mutation_id};

    const ShaderCompilerOutput* temp_output = GlobalShaderCache::GetInstance().FindShaderCache((EShaderPlatform)input.target_info.shader_platform, key);

    bool valid_cache = (temp_output != nullptr && temp_output->source_file_last_write_time == _last_write_time);

    if (valid_cache) {
        output = *temp_output;
        return true;
    }

    return false;
}

ShaderCompilerOutput* DXCompiler::CompileVulkan(const ShaderCompilerInput& _input) {
    ShaderCompilerOutput* output_ptr = MoerNew(ShaderCompilerOutput)();
    auto&                 output     = *output_ptr;

    auto on_fail = [&output, _input](std::string message) mutable {
        output.errors.push_back(std::move(message));
        output.b_succeeded = false;
        output.target_info = _input.target_info;
    };

    std::string        file_name_c(_input.relative_source_file_path);
    std::string        entry_name_c(_input.entry_point);
    const std::wstring entry_name = std::wstring(entry_name_c.begin(), entry_name_c.end());

    HRESULT hres;

    std::filesystem::path file_path    = Moer::ConfigManager::GetInstance().GetEngineShaderPath();
    std::string           platform_str = ToString((EShaderPlatform)_input.target_info.shader_platform);
    if (platform_str.empty()) {
        on_fail("platform not supported");
        return output_ptr;
    }

    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    if (!std::filesystem::exists(file_path)) {
        on_fail(std::format("Could not load shader file: {}", std::move(file_path.generic_string())));

        return output_ptr;
    }

    std::filesystem::file_time_type last_write_time = std::filesystem::last_write_time(file_path);

    if (LoadCache(last_write_time.time_since_epoch().count(), _input, output)) {
        return output_ptr;
    }

    // Load the HLSL text shader from disk
    uint32_t                 code_page = DXC_CP_ACP;
    ComPtr<IDxcBlobEncoding> source_blob;

    //Read source file
    std::ifstream     file_stream(file_path);
    std::stringstream file_source_stream;
    file_source_stream << file_stream.rdbuf();
    file_stream.close();

    if (file_source_stream.str().empty()) {
        on_fail(std::move(std::format("shader file {} is empty", file_path.generic_string())));
        return output_ptr;
    }
    hres = utils->CreateBlob(file_source_stream.str().data(), file_source_stream.str().size(), CP_UTF8, source_blob.GetAddressOf());

    if (FAILED(hres)) {
        on_fail("Could not load shader file");
        return output_ptr;
    }

    // Select target profile based on shader file extension
    auto target_profile = GetPlatform((EShaderType)_input.target_info.shader_type, (EShaderPlatform)_input.target_info.shader_platform);

    size_t idx = file_name.rfind('.');

    if (idx != std::string::npos) {
        // todo: check file idx to match target
    }

    const auto&  defines = _input.environment.GetDefines();
    std::wstring defines_str;

    const auto& compile_args = _input.environment.GetCompilerArgs();

    for (const auto& arg : compile_args) {
        std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(arg.first.begin(), arg.first.end());

        auto temp_second = ShaderCompilerEnvironment::GetVariantWStr(arg.second);

        defines_str.append(std::format(L"-D{}={}", temp_first, temp_second));
    }

    for (const auto& define : defines) {
        std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(define.first.begin(), define.first.end());
        std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_second(define.second.begin(), define.second.end());

        defines_str.append(std::format(L"-D{}={}", temp_first, temp_second));
    }

    std::wstring included_path = Moer::ConfigManager::GetInstance().GetEngineShaderPath().generic_wstring();

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
        L"-I",
        included_path.c_str(),
        // Compile to SPIRV
        L"-spirv",
        defines_str.c_str(),
        L"-fspv-target-env=vulkan1.3",

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
        include_handler.Get(),
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
            output.errors.push_back((const char*)error_blob->GetBufferPointer());
            return output_ptr;
        }
    }
    // Get compilation result
    IDxcBlob* code;
    result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();

    output.b_succeeded      = true;
    output.shader_name_hash = _input.shader_name_hash;
    output.shader_code.resize(size);
    output.compiled_hash.FromData(data, size);
    output.mutation_id = _input.mutation_id;

    memcpy(&output.shader_code[0], data, size);

    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
    reflector->ReflectShader(result.Get(), _input.param_meta_data, param_map);
    output.parameter_map.param_map.swap(param_map);
    output.target_info                 = _input.target_info;
    output.cached                      = false;
    output.source_file_last_write_time = last_write_time.time_since_epoch().count();

    // StoreCache(last_write_time.time_since_epoch().count(), _input, _output);
    return output_ptr;
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
