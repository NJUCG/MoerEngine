#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"
#include <cassert>
#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
// #include <zpp_bits.h>
#endif

#include "rhi/RHI.h"
#include <wrl/client.h>
// #include "wsl/wrladapter.h"

#include <filesystem>
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include "DXCUtils.h"
#include "spirv_reflect.h"
#include <format>
#include <fstream>
#include <sstream>
#include <stdint.h>
#include <string>
#include <string_view>

#include "dxc/dxcapi.h"
#include "shader/ShaderCommon.h"
#include "DirectXShaderReflectorVulkan.h"

// std::function<ShaderCompilerOutput*(const ShaderCompilerInput& input)> DXCompiler::s_compiler_func_table[EShaderPlatform::SP_Num]{};

using Microsoft::WRL::ComPtr;

struct DXCompiler::Impl {
    Impl();
    ~Impl();

private:
    friend class DXCompiler;
    ComPtr<IDxcCompiler3>      compiler        = nullptr;
    ComPtr<IDxcLibrary>        library         = nullptr;
    ComPtr<IDxcUtils>          utils           = nullptr;
    ComPtr<IDxcIncludeHandler> include_handler = nullptr;

    Moer::UniquePtr<ShaderReflector> reflector;

    void Compile(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output);

    void ReflectSPIRV(ComPtr<IDxcResult> result, const ShaderParametersMetadata* _meta_param, ShaderParametersInfoMap& _param_map);
};

DXCompiler::Impl::Impl() {
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
}

DXCompiler::Impl::~Impl() {
    utils           = nullptr;
    include_handler = nullptr;
    library         = nullptr;
    compiler        = nullptr;
}

// static ComPtr<IDxcCompiler3>      compiler        = nullptr;
// static ComPtr<IDxcValidator>      validator       = nullptr;
// static ComPtr<IDxcLibrary>        library         = nullptr;
// static ComPtr<IDxcUtils>          utils           = nullptr;
// static ComPtr<IDxcIncludeHandler> include_handler = nullptr;

DXCompiler& DXCompiler::GetInstance() {
    static DXCompiler s_compiler;
    return s_compiler;
}
DXCompiler::~DXCompiler() {
    // utils->Release();
    // library->Release();
    // compiler->Release();
    // delete reflector;
    MoerDelete(impl);
}

DXCompiler::DXCompiler() {
    impl = MoerNew(Impl)();
    // // Initialize DXC library
    // HRESULT hres = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
    // if (FAILED(hres)) {
    //     LOG_ERROR("dxcompiler library load fail.");
    //     return;
    // }
    // library->CreateIncludeHandler(&include_handler);

    // hres = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    // if (FAILED(hres)) {
    //     LOG_ERROR("Could not init DXC Compiler");
    //     return;
    // }

    // // Initialize DXC utility
    // hres = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    // if (FAILED(hres)) {
    //     LOG_ERROR("Could not init DXC Utiliy");
    //     return;
    // }
    // utils->AddRef();
    // library->AddRef();
    // compiler->AddRef();
    // include_handler->AddRef();

    // s_compiler_func_table[SP_WIN_D3D_SM6] = std::bind(&DXCompiler::CompileD3D12, this, std::placeholders::_1);
    // s_compiler_func_table[SP_VULKAN_SM6]  = std::bind(&DXCompiler::CompileVulkan, this, std::placeholders::_1);

    // if (g_rhi->GetType() == ERHIType::Vulkan)
    //     reflector = new DirectXShaderReflectorVulkan();
}
bool LoadCache(long long _last_write_time, const ShaderCompilerInput& input, ShaderCompilerOutput& output);
void DXCompiler::Impl::Compile(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {

    auto push_back_error_message = [&_output](std::string message) {
        _output.errors.push_back(std::move(message));
        _output.b_succeeded = false;
    };

    auto add_dx_arg = [](Moer::Array<std::wstring>& arguments) {
        //add_no_arg_for_dx
    };

    auto add_vk_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.3");
        arguments.push_back(L"-fvk-use-dx-position-w");
        arguments.push_back(L"-fvk-use-dx-layout");
        arguments.push_back(L"-fvk-auto-shift-bindings");
        // arguments.push_back(L"-fspv-flatten-resource-arrays");
    };

    auto set_default_args = [add_dx_arg, add_vk_arg](Moer::Array<std::wstring>& arguments, EShaderPlatform _platform, EShaderType _type, std::string_view _entry_point) {
        arguments.push_back(L"-T");

        arguments.push_back(GetPlatform(_type, _platform));
        arguments.push_back(L"-E");
        // std::wstring entry_point(_entry_point.begin(), _entry_point.end());
        arguments.emplace_back(std::wstring(_entry_point.begin(), _entry_point.end()));
        arguments.push_back(L"-I");
        arguments.push_back(Moer::ConfigManager::GetInstance().GetEngineShaderPath().generic_wstring());
        arguments.push_back(L"-Zpr");
        // arguments.push_back(L"-all-resources-bound");
        if (_platform == SP_WIN_D3D_SM6)
            add_dx_arg(arguments);
        else if (_platform == SP_VULKAN_SM6)
            add_vk_arg(arguments);
    };
    auto add_debug_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(DXC_ARG_ALL_RESOURCES_BOUND);
        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
    };

    auto add_define_arg = [](Moer::Array<std::wstring>& arguments, const Moer::UnorderedMap<std::string, std::string>& _defines) {
        Moer::Array<DxcDefine> defines(_defines.size());

        for (const auto& define : _defines) {
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(define.first.begin(), define.first.end());
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_second(define.second.begin(), define.second.end());
            arguments.push_back(std::format(L"-D{}={}", temp_first, temp_second));
        }
    };

    auto add_compile_arg = [](Moer::Array<std::wstring>& arguments, decltype(_input.environment.GetCompilerArgs()) _compile_args) {
        for (const auto& arg : _compile_args) {
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(arg.first.begin(), arg.first.end());
            auto                                                                                temp_second = ShaderCompilerEnvironment::GetVariantWStr(arg.second);
            arguments.push_back(std::format(L"-D{}={}", temp_first, temp_second));
        }
    };

    auto root_path       = Moer::ConfigManager::GetInstance().GetEngineShaderPath();
    auto file_path       = std::filesystem::canonical(root_path / _input.relative_source_file_path);
    auto last_write_time = std::filesystem::last_write_time(file_path);

    if (LoadCache(last_write_time.time_since_epoch().count(), _input, _output)) {
        return;
    }

    Moer::Array<std::wstring> arguments = {file_path.generic_wstring().c_str()};

    set_default_args(arguments, (EShaderPlatform)_input.target_info.shader_platform, (EShaderType)_input.target_info.shader_type, _input.entry_point);
#if _DEBUG
    add_debug_arg(arguments);
#endif
    add_compile_arg(arguments, _input.environment.GetCompilerArgs());
    add_define_arg(arguments, _input.environment.GetDefines());

    {
        auto read_data_from_file = [](const std::filesystem::path& _file_path) -> std::string {
            std::ifstream     file_stream(_file_path);
            std::stringstream file_source_stream;
            file_source_stream << file_stream.rdbuf();
            file_stream.close();
            return file_source_stream.str();
        };
        ComPtr<IDxcResult> result{nullptr};

        ComPtr<IDxcBlobEncoding> source_blob;
        //replace in async io service later on
        auto file_data = read_data_from_file(file_path);
        utils->CreateBlob(file_data.data(), file_data.size(), CP_UTF8, &source_blob);
        DxcBuffer buffer{
            .Ptr      = source_blob->GetBufferPointer(),
            .Size     = source_blob->GetBufferSize(),
            .Encoding = DXC_CP_ACP,
        };
        Moer::Array<LPWSTR> arguments_wchar(arguments.size());
        for (size_t i = 0; i < arguments.size(); ++i) {
            arguments_wchar[i] = arguments[i].data();
        }
        HRESULT hres = compiler->Compile(
            &buffer,
            (LPCWSTR*)arguments_wchar.data(),
            (uint32_t)arguments_wchar.size(),
            include_handler.Get(),
            IID_PPV_ARGS(&result));

        if (SUCCEEDED(hres)) {
            result->GetStatus(&hres);
        }

        if (FAILED(hres) && (result)) {
            ComPtr<IDxcBlobEncoding> error_blob;
            hres = result->GetErrorBuffer(&error_blob);
            std::stringstream error_stream;
            if (SUCCEEDED(hres) && error_blob) {

                error_stream << "Shader compilation failed :\n"
                             << (const char*)error_blob->GetBufferPointer();
                push_back_error_message((const char*)error_blob->GetBufferPointer());
                return;
            }
        }

        if (_input.target_info.shader_platform == SP_VULKAN_SM6) {
            ReflectSPIRV(result, _input.param_meta_data, _output.parameter_map);
        } else {
            //reflect dx12
            assert(false && "dx reflect not implemented");
        }

        auto fill_succuss_data = [&_output, &last_write_time, &file_path, &result, &file_data, &_input]() {
            IDxcBlob* code;
            result->GetResult(&code);
            const uint8_t* data = (uint8_t*)code->GetBufferPointer();
            uint32_t       size = code->GetBufferSize();
            _output.shader_code.resize(size);
            std::copy(data, data + size, _output.shader_code.begin());

            _output.b_succeeded      = true;
            _output.shader_name_hash = _input.shader_name_hash;
            _output.compiled_hash.FromData(data, size);
            _output.mutation_id                 = _input.mutation_id;
            _output.target_info                 = _input.target_info;
            _output.cached                      = false;
            _output.source_file_last_write_time = last_write_time.time_since_epoch().count();
        };

        fill_succuss_data();
    }
}

ShaderCompilerOutput* DXCompiler::Compile(const ShaderCompilerInput& _input) {
    auto* output_ptr = MoerNew(ShaderCompilerOutput)();
    impl->Compile(_input, *output_ptr);
    return output_ptr;
}

ShaderCompilerOutput DXCompiler::Compile(ShaderCompilerInput&& _input) {
    ShaderCompilerOutput output;
    impl->Compile(_input, output);
    return std::move(output);
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
        case ST_RAY_CLOSESTHIT:
        case ST_RAY_CALLABLE:
        case ST_RAY_ANYHIT:
        case ST_RAY_INTERSECTION:
            b_support_shader_type = true;
            break;
        case ST_Num: break;
        default: break;
    }
    return b_support_platform && b_support_shader_type;
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

void DXCompiler::Impl::ReflectSPIRV(ComPtr<IDxcResult> result, const ShaderParametersMetadata* _meta_param, ShaderParametersInfoMap& _param_map) {
    IDxcBlob* code;
    result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();

    SpvReflectShaderModule reflect_module;
    SpvReflectResult       ref_result = spvReflectCreateShaderModule(size, data, &reflect_module);
    assert(ref_result == SPV_REFLECT_RESULT_SUCCESS);

    Moer::Array<SpvReflectInterfaceVariable*> input_vars(reflect_module.input_variable_count);
    spvReflectEnumerateInputVariables(&reflect_module, &reflect_module.input_variable_count, input_vars.data());

    ShaderReflectInfo reflect_info;
    auto&             vertex_inputs = reflect_info.vertex_input_info;

    Moer::UnorderedMap<std::string, ParameterInfo>
                                    param_map;
    const ShaderParametersMetadata* meta_data = _meta_param;

    for (uint32_t binding_index = 0; binding_index < reflect_module.descriptor_binding_count; ++binding_index) {
        auto& binding = reflect_module.descriptor_bindings[binding_index];
        auto& param   = param_map[binding.name];
        param.slot    = binding.binding;
        param.space   = binding.set;
        param.type    = ToShaderParameterType(binding.resource_type);
        param.stage |= ToPipelineStageFlag(reflect_module.shader_stage);
        param.type_flags = binding.resource_type | binding.descriptor_type << 4u;
        param.num        = binding.count;
    }
    for (uint32_t push_constant_index = 0; push_constant_index < reflect_module.push_constant_block_count; ++push_constant_index) {
        auto& push_constant = reflect_module.push_constant_blocks[push_constant_index];
        auto& param         = param_map[push_constant.name];
        param.slot          = -1;
        param.space         = -1;
        param.type          = EShaderParameterType::CONSTANT_STRUCT;
        param.stage |= ToPipelineStageFlag(reflect_module.shader_stage);
        param.type_flags = push_constant.padded_size | 1u << 20u;
        param.num        = push_constant.size;
    }
    const auto&              members = meta_data->GetMembers();
    Moer::Array<std::string> error_msgs;
#if _DEBUG
    for (const ShaderParametersMetadata::Member& member : members) {
        EShaderBindingBaseType base_type = member.GetBaseType();
        std::string_view       name      = member.GetName();

        auto entry = param_map.find(name.data());
        auto end   = param_map.end();
        auto count = param_map.count(name.data());

        if (count <= 0) {
            {
                error_msgs.push_back(std::format("param {} not found in shader {}", member.GetName(), reflect_module.entry_point_name));
            }
            continue;
        }
        //param reflected
        const auto& param            = entry->second;
        auto        cpp_binding_type = BindingTypeToParameterType(base_type);
        if (cpp_binding_type != param.type) {
            //type mismatch
            if (base_type == SBT_CONST_STRUCT) {
                error_msgs.push_back(std::format("push constant member define error, should be writen as\n"
                                                 "\t[[vk::push_constant]]\nConstantBuffer<YourConstantStruct> {};",
                                                 member.GetName()));
                continue;
            }
            error_msgs.push_back(std::format("param {} format mismatch! param format: {}, shader format {}", member.GetName(), ToString(base_type), ToString(param.type)));
            continue;
        }
        // LOG_INFO("param {}: {{ slot:{}, set:{}, array_num:{} }}", member.GetName(), param.slot, param.space, param.num);
    }

    for (const auto& msg : error_msgs) {
        LOG_WARNING(msg);
    }
    if (!error_msgs.empty()) {
        // assert(false && "shader reflection error");
    }
#endif
    _param_map.param_map.swap(param_map);
    _param_map.space_cnt = reflect_module.descriptor_set_count;
    spvReflectDestroyShaderModule(&reflect_module);
}
