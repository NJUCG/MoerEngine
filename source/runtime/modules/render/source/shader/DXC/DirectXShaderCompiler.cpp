#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"

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

std::function<void(const ShaderCompilerInput& input, ShaderCompilerOutput& output)> DXCompiler::s_compiler_func_table[EShaderPlatform::SP_Num]{};

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
static ComPtr<IDxcCompiler3> compiler  = nullptr;
static ComPtr<IDxcValidator> validator = nullptr;
static ComPtr<IDxcLibrary>   library   = nullptr;
static ComPtr<IDxcUtils>     utils     = nullptr;
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

bool LoadCache(long long _last_write_time, const ShaderCompilerInput& input, ShaderCompilerOutput& output) {
    std::filesystem::path output_path  = Moer::ConfigManager::GetInstance().GetEngineShaderCachedPath();
    std::string           platform_str = ToString(input.target_info.shader_platform);

    output_path /= platform_str;
    output_path /= input.relative_source_file_path;
    std::string extension = output_path.extension().generic_string().substr(1);
    std::string file_name = output_path.filename().generic_string().substr(0, output_path.filename().generic_string().size() - extension.size() - 1);
    output_path.replace_filename(std::format("{}_{}_mut{}.cache", file_name, extension, input.mutation_id));

    if (std::filesystem::exists(output_path)) {
        Moer::ShaderCompiledCacheFile cache_file;
        std::ifstream                 ifs(output_path, std::ios::binary);
        std::stringstream             file_str;
        file_str << ifs.rdbuf();

        std::string_view file_view = file_str.rdbuf()->view();
        auto             in        = zpp::bits::in(file_view);
        in(cache_file).or_throw();

        if (cache_file.Equals(input) && cache_file.GetLastWriteTime() == _last_write_time) {
            output.b_succeeded = true;
            output.shader_name = input.shader_name;
            output.shader_code.swap(cache_file.code);
            output.compiled_hash = *(Hash64City*)&cache_file.hash;
            output.mutation_id   = input.mutation_id;
            output.parameter_map.param_map.swap(cache_file.param_map);
            output.target_info = input.target_info;
            return true;
        }
    }
    return false;
}

void StoreCache(long long _last_write_time, const ShaderCompilerInput& input, const ShaderCompilerOutput& _output) {
    std::filesystem::path cache_file_path = Moer::ConfigManager::GetInstance().GetEngineShaderCachedPath();
    std::string           platform_str    = ToString(input.target_info.shader_platform);

    cache_file_path /= platform_str;
    cache_file_path /= input.relative_source_file_path;
    std::string extension = cache_file_path.extension().generic_string().substr(1);
    std::string file_name = cache_file_path.filename().generic_string().substr(0, cache_file_path.filename().generic_string().size() - extension.size() - 1);
    cache_file_path.replace_filename(std::format("{}_{}_mut{}.cache", file_name, extension, input.mutation_id));
    //create cache file
    Moer::ShaderCompiledCacheFile cache_file;
    cache_file.hash            = *((uint64_t*)&_output.compiled_hash.hash_code[0]);
    cache_file.mutation_id     = _output.mutation_id;
    cache_file.platform        = _output.target_info.shader_platform;
    cache_file.shader_type     = _output.target_info.shader_type;
    cache_file.last_write_time = _last_write_time;
    cache_file.code.resize(_output.shader_code.size());
    memcpy(&cache_file.code[0], &_output.shader_code[0], _output.shader_code.size());
    cache_file.param_map = _output.parameter_map.param_map;

    if (!std::filesystem::exists(cache_file_path.parent_path()))
        std::filesystem::create_directories(cache_file_path.parent_path());
    std::fstream         ofs(cache_file_path, std::ios::binary | std::ios::out);
    Moer::Array<uint8_t> out_stream;
    auto                 out = zpp::bits::out(out_stream);
    out(cache_file).or_throw();

    ofs.write((const char*)out_stream.data(), out_stream.size());
}

void DXCompiler::CompileVulkan(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
    static auto on_fail = [&](const char* messsage) {
        _output.errors.push_back(messsage);
        _output.b_succeeded = false;
        _output.target_info = _input.target_info;
    };

    std::string        file_name_c(_input.relative_source_file_path);
    std::string        entry_name_c(_input.entry_point);
    const std::wstring entry_name = std::wstring(entry_name_c.begin(), entry_name_c.end());

    HRESULT hres;

    std::filesystem::path file_path = Moer::ConfigManager::GetInstance().GetEngineShaderPath();

    std::filesystem::path output_path  = Moer::ConfigManager::GetInstance().GetEngineShaderCachedPath();
    std::string           platform_str = ToString(_input.target_info.shader_platform);
    if (platform_str.empty()) {
        on_fail("platform not supported");
        return;
    }

    file_path /= file_name_c;

    std::wstring file_name = file_path.generic_wstring();

    if (!std::filesystem::exists(file_path)) {
        on_fail(std::format("Could not load shader file: {}", file_path.generic_string()).c_str());

        return;
    }

    auto platform_output_dir = output_path / platform_str;

    std::filesystem::path cache_file_path = (platform_output_dir / file_name_c).replace_extension(std::format(".cache{}", _input.mutation_id));

    std::filesystem::file_time_type last_write_time = std::filesystem::last_write_time(file_path);

    if (LoadCache(last_write_time.time_since_epoch().count(), _input, _output))
        return;

    // Load the HLSL text shader from disk
    uint32_t                 code_page = DXC_CP_ACP;
    ComPtr<IDxcBlobEncoding> source_blob;

    //Read source file
    std::ifstream     file_stream(file_path);
    std::stringstream file_source_stream;
    file_source_stream << file_stream.rdbuf();
    file_stream.close();

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
    _output.shader_name = _input.shader_name;
    _output.shader_code.resize(size);
    _output.compiled_hash.FromData(data, size);
    _output.mutation_id = _input.mutation_id;

    memcpy(&_output.shader_code[0], data, size);

    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
    reflector->ReflectShader(result.Get(), _input.param_meta_data, param_map);
    _output.parameter_map.param_map.swap(param_map);
    _output.target_info = _input.target_info;

    StoreCache(last_write_time.time_since_epoch().count(), _input, _output);
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
