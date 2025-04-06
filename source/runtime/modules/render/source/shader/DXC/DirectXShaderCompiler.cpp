#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResource.h"
#include "shader/ShaderResourceManager.h"
#include "spirv.hpp"
#include "spirv_common.hpp"
#include <cassert>
#include <optional>
#include <variant>
#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif

#include "rhi/RHI.h"
#include <wrl/client.h>

#include <filesystem>
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include "DXCUtils.h"
#include <format>
#include <fstream>
#include <sstream>
#include <stdint.h>
#include <string>
#include <string_view>

#include "dxc/dxcapi.h"
#include "shader/ShaderCommon.h"
#include "spirv_cross.hpp"

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
    void ReflectDXIL(ComPtr<IDxcResult> result, const ShaderCompilerInput& _input, ShaderParametersInfoMap& _param_map);
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
        arguments.push_back(L"-DDXIL=1");
    };

    auto add_vk_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.3");
        arguments.push_back(L"-fvk-use-dx-position-w");
        arguments.push_back(L"-fvk-use-dx-layout");
        arguments.push_back(L"-fvk-auto-shift-bindings");
        arguments.push_back(L"-fspv-preserve-interface");
        arguments.push_back(L"-DVULKAN=1");
        // arguments.push_back(L"-fspv-flatten-resource-arrays");

        // - new dxc(like https://www.nuget.org/packages/Microsoft.Direct3D.DXC/1.8.2502.8) support ResourceDescriptorHeap for spirv
        //   and require extra extension/capability for RT.
        //   however now meet bug https://github.com/microsoft/DirectXShaderCompiler/issues/7181. seems fixed, but not release yet.
        // - for dx, dxc output dxil by default, but not preserve inactive resource binding. while spirv can.
        //   now still use dxil for dx due to above bug
        //arguments.push_back(L"-fspv-extension=SPV_EXT_descriptor_indexing");
        //arguments.push_back(L"-fspv-extension=SPV_KHR_ray_tracing");
        //arguments.push_back(L"-fspv-extension=SPV_KHR_ray_query");
    };

    auto set_default_args = [add_dx_arg, add_vk_arg](Moer::Array<std::wstring>& arguments, EShaderPlatform _platform, EShaderType _type, std::string_view _entry_point) {
        arguments.push_back(L"-T");

        arguments.push_back(GetPlatform(_type, _platform));
        arguments.push_back(L"-E");
        // std::wstring entry_point(_entry_point.begin(), _entry_point.end());
        arguments.emplace_back(std::wstring(_entry_point.begin(), _entry_point.end()));
        arguments.push_back(L"-I");
        arguments.push_back(Moer::ConfigManager::GetInstance().GetEngineShaderPath().generic_wstring());
        arguments.push_back(L"-I");
        arguments.push_back(Moer::ConfigManager::GetInstance().GetEngineShaderSharedPath().generic_wstring());
        // arguments.push_back(L"-Zpr");
        // arguments.push_back(L"-all-resources-bound");
        if (_platform == SP_WIN_D3D_SM6)
            add_dx_arg(arguments);
        else if (_platform == SP_VULKAN_SM6)
            add_vk_arg(arguments);
    };
    auto add_debug_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(DXC_ARG_ALL_RESOURCES_BOUND);
        arguments.push_back(DXC_ARG_OPTIMIZATION_LEVEL0);
        // arguments.push_back(DXC_ARG_SKIP_OPTIMIZATIONS);
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

    //if (LoadCache(last_write_time.time_since_epoch().count(), _input, _output)) {
    //    return;
    //}

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
            //dx12, reflect through dxil
            ReflectDXIL(result, _input, _output.parameter_map);
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

    ShaderReflectInfo reflect_info;
    auto&             vertex_inputs = reflect_info.vertex_input_info;

    Moer::UnorderedMap<std::string, ParameterInfo>
                                    param_map;
    const ShaderParametersMetadata* meta_data = _meta_param;
    using namespace Moer;

    constexpr std::string_view bdles_suffix      = "_114514_bdls";
    constexpr std::string_view bdls_array_suffix = "_array_114514_bdls";
    auto                       is_bdls           = [&](const std::string& _name) {
        return _name.ends_with(bdles_suffix);
    };
    auto get_real_name = [](const std::string& _name) {
        return _name.substr(0, _name.find_first_of("__"));
    };
    auto is_bdls_array = [&](const std::string& _name) {
        return _name.ends_with(bdls_array_suffix);
    };
#define SetZeroIfEmpty(_param) \
    if (!_param.has_value()) _param = ReflectParamInfo::Bindless();

    Moer::UnorderedMap<std::string, Moer::ReflectParamInfo> reflect_map;

    {
        std::span<Moer::uint> spirv_code_span((Moer::uint*)data, size / sizeof(Moer::uint));
        //spirv-cross test
        spirv_cross::Compiler        comp(spirv_code_span.data(), spirv_code_span.size());
        spirv_cross::ShaderResources resources = comp.get_shader_resources();

        auto active     = comp.get_active_interface_variables();
        auto active_res = comp.get_shader_resources(active);

        Moer::UnorderedSet<std::string> active_res_names;
        for (auto& res : active_res.separate_images) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.storage_images) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.storage_buffers) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.uniform_buffers) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.push_constant_buffers) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.acceleration_structures) {
            active_res_names.insert(comp.get_name(res.id));
        }
        for (auto& res : active_res.separate_samplers) {
            active_res_names.insert(comp.get_name(res.id));
        }

        auto res_is_active = [&](spirv_cross::Resource& _res) {
            auto name = comp.get_name(_res.id);
            return active_res_names.contains(name);
        };

#define GET_RESOURCE_DEFAULT_INFOS(resource)                                         \
    auto set       = comp.get_decoration(resource.id, spv::DecorationDescriptorSet); \
    auto binding   = comp.get_decoration(resource.id, spv::DecorationBinding);       \
    auto name      = comp.get_name(resource.id);                                     \
    auto type      = comp.get_type(resource.base_type_id);                           \
    auto count     = type.array.size() > 0 ? type.array[0] : 1;                      \
    auto is_active = res_is_active(resource)

        auto handle_bdls_res_tex = [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
            GET_RESOURCE_DEFAULT_INFOS(_res);
            static constexpr std::string_view real_name  = ReflectParamInfo::bdls_name;
            ReflectParamInfo::BindlessArray&  bdls_param = reflect_map[real_name.data()].spirv.bindless;
            ReflectParamInfo::Bindless*       target     = nullptr;

            if (_desc_type == VDT_STORAGE_BUFFER) {
                if (is_bdls_array(name)) {
                    SetZeroIfEmpty(bdls_param.array);
                    target = &bdls_param.array.value();
                } else {
                    SetZeroIfEmpty(bdls_param.buffer);
                    target = &bdls_param.buffer.value();
                }
            } else if (_desc_type == VDT_SAMPLED_IMAGE) {
                SetZeroIfEmpty(bdls_param.image);
                target = &bdls_param.image.value();

            } else if (_desc_type == VDT_SAMPLER) {
                SetZeroIfEmpty(bdls_param.sampler);
                target = &bdls_param.sampler.value();
            } else if (_desc_type == VDT_ACCELERATION_STRUCTURE) {
                SetZeroIfEmpty(bdls_param.acceleration_structure);
                target = &bdls_param.acceleration_structure.value();
            } else {
                LOG_INFO("unknown bindless type {}", uint(_desc_type));
                assert(false && "unknown bindless type");
            }
            target->set           = set;
            target->binding       = binding;
            target->count         = count;
            target->desc_type     = _desc_type;
            target->resource_type = _srt;
            target->stage_bits    = uint(ToPipelineStageFlag(comp.get_execution_model()));
            target->custom_flag.active |= is_active;
            // if (is_active) {
            //     LOG_INFO("active bdls name : {}", name);
            // }
        };

        auto handle_res = [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
            GET_RESOURCE_DEFAULT_INFOS(_res);
            ReflectParamInfo& param = reflect_map[name];
            param.spirv.resources.stage_bits |= uint(ToPipelineStageFlag(comp.get_execution_model()));
            ReflectParamInfo::Resource res{};
            res.set                    = set;
            res.binding                = binding;
            res.sampled                = type.image.sampled;
            res.desc_type              = _desc_type;
            res.resource_type          = _srt;
            res.count                  = count;
            res.format                 = ToPixelFormat(type.image.format);
            res.custom_flag.active     = is_active;
            param.spirv.resources.data = res;
        };

        auto handle_all_res = [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
            if (is_bdls(_res.name)) {
                handle_bdls_res_tex(_res, _desc_type, _srt);
            } else {
                handle_res(_res, _desc_type, _srt);
            }
        };

        for (auto& resource : resources.storage_images) {
            auto name = comp.get_name(resource.id);
            auto type = comp.get_type(resource.base_type_id);

            if (type.image.dim == spv::DimBuffer) {
                handle_res(resource, VDT_STORAGE_TEXEL_BUFFER, EShaderResourceType::SRT_UAV);

            } else {
                handle_all_res(resource, VDT_STORAGE_IMAGE, EShaderResourceType::SRT_UAV);
            }
        }

        for (auto& resource : resources.separate_images) {
            auto type = comp.get_type(resource.base_type_id);
            if (type.image.dim == spv::DimBuffer) {
                // LOG_INFO("Buffer name : {} set : {} binding : {}", name, set, binding);
                handle_res(resource, VDT_UNIFORM_TEXEL_BUFFER, EShaderResourceType::SRT_SRV);
            } else {
                // LOG_INFO("Texture name : {} set : {} binding : {}", name, set, binding);
                handle_all_res(resource, VDT_SAMPLED_IMAGE, EShaderResourceType::SRT_SRV);
            }
        }

        for (auto& resource : resources.separate_samplers) {
            handle_all_res(resource, VDT_SAMPLER, EShaderResourceType::SRT_SAMPLER);
        }

        for (auto& resource : resources.storage_buffers) {
            spirv_cross::Bitset buffer_flags = comp.get_buffer_block_flags(resource.id);
            // GET_RESOURCE_DEFAULT_INFOS(resource);
            // LOG_INFO("storage buffer name : {} set : {} binding : {}", name, set, binding);
            handle_all_res(resource, VDT_STORAGE_BUFFER, buffer_flags.get(spv::DecorationNonWritable) ? EShaderResourceType::SRT_SRV : EShaderResourceType::SRT_UAV);
        }

        for (auto& resource : resources.uniform_buffers) {
            handle_all_res(resource, VDT_UNIFORM_BUFFER, EShaderResourceType::SRT_CBV);
        }

        for (auto& resource : resources.push_constant_buffers) {

            GET_RESOURCE_DEFAULT_INFOS(resource);
            auto ranges = comp.get_active_buffer_ranges(resource.id);
            auto block  = comp.get_decoration(resource.base_type_id, spv::DecorationBufferBlock);

            uint  offset                     = 0;
            uint  constant_size              = comp.get_declared_struct_size(type);
            auto& param                      = reflect_map[name];
            param.spirv.resources.stage_bits = uint(ToPipelineStageFlag(comp.get_execution_model()));
            ReflectParamInfo::Constant constant{};
            constant.size               = constant_size;
            constant.padded_size        = constant_size;
            constant.offset             = offset;
            constant.custom_flag.active = is_active;
            param.spirv.resources.data  = constant;
        }

        //accel
        for (auto& resource : resources.acceleration_structures) {
            handle_all_res(resource, VDT_ACCELERATION_STRUCTURE, SRT_SRV);
        }
    }
#undef SetZeroIfEmpty

#undef GET_RESOURCE_DEFAULT_INFOS

    _param_map.reflect_map.swap(reflect_map);
}

#include <d3d12shader.h>

#define DX_CHECK_HRESULT(hr)                                         \
    do {                                                             \
        HRESULT _hr = (hr);                                          \
        if (_hr < 0) {                                               \
            LOG_CRITICAL("ERROR: hresult={:#x}", (unsigned int)_hr); \
            std::terminate();                                        \
        }                                                            \
    } while (0)

void DXCompiler::Impl::ReflectDXIL(ComPtr<IDxcResult> result, const ShaderCompilerInput& _input, ShaderParametersInfoMap& _param_map) {
    // Get shader reflection data.
    ComPtr<IDxcBlob> reflectionBlob{};
    DX_CHECK_HRESULT(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflectionBlob), nullptr));

    const DxcBuffer reflectionBuffer{
        .Ptr      = reflectionBlob->GetBufferPointer(),
        .Size     = reflectionBlob->GetBufferSize(),
        .Encoding = 0,
    };

    ComPtr<ID3D12ShaderReflection> shaderReflection{};
    utils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(&shaderReflection));
    D3D12_SHADER_DESC shaderDesc{};
    shaderReflection->GetDesc(&shaderDesc);

    // todo InputParameters for vs, inputlayout ?

    Moer::UnorderedMap<std::string, Moer::ReflectParamInfo> reflect_map;

    constexpr std::string_view bdls_suffix = "_114514_bdls";

    for (int i = 0; i < shaderDesc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC shaderInputBindDesc{};
        DX_CHECK_HRESULT(shaderReflection->GetResourceBindingDesc(i, &shaderInputBindDesc));

        auto& param_info = reflect_map[shaderInputBindDesc.Name].dxil;
        param_info.slot  = shaderInputBindDesc.BindPoint;
        param_info.space = shaderInputBindDesc.Space;
        param_info.count = shaderInputBindDesc.BindCount;

        if (param_info.count == 0) {// Texture2D<float4> xxx[]; -> count=0
            if (!std::string(shaderInputBindDesc.Name).ends_with(bdls_suffix)) {
                LOG_ERROR("bindless shader resource '{}' should end with '_114514_bdls' in '{}'", shaderInputBindDesc.Name, _input.shader_name);
            }
        }

        switch (shaderInputBindDesc.Type) {
            case D3D_SIT_CBUFFER: {
                // now suppose to be constant buffer, maybe change to root constant later.
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::ConstantBuffer;
                auto* cb        = shaderReflection->GetConstantBufferByName(shaderInputBindDesc.Name);
                assert(cb);
                D3D12_SHADER_BUFFER_DESC desc{};
                DX_CHECK_HRESULT(cb->GetDesc(&desc));
                param_info.byte_size = desc.Size;
            } break;
            case D3D_SIT_TEXTURE: {
                const auto dim = shaderInputBindDesc.Dimension;
                switch (dim) {
                    case D3D_SRV_DIMENSION_BUFFER:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::TypedBuffer;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture1D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture1DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture2D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture2DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DMS:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture2DMS;
                        //num_sample = shaderInputBindDesc.NumSamples;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture2DMSArray;
                        //num_sample = shaderInputBindDesc.NumSamples;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Texture3D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURECUBE:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::TextureCube;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::TextureCubeArray;
                        break;
                    default:
                        LOG_WARNING("unsupported shader varibale '{}' dimension: '{}' in shader '{}'", shaderInputBindDesc.Name, uint32_t(shaderInputBindDesc.Type), _input.shader_name);
                        assert(false && "unsupported type");
                        break;
                }
            } break;
            case D3D_SIT_SAMPLER:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::Sampler;
                break;
            case D3D_SIT_UAV_RWTYPED: {
                // todo? Shader Model 6.7 introduces writable multi-sampled texture resource. https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_7_Advanced_Texture_Ops.html#writable-msaa-textures
                const auto dim = shaderInputBindDesc.Dimension;
                switch (dim) {
                    case D3D_SRV_DIMENSION_BUFFER:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTypedBuffer;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTexture1D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTexture1DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTexture2D;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTexture2DArray;
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWTexture3D;
                        break;
                    default:
                        LOG_WARNING("unsupported shader varibale '{}' rw dimension: '{}' in shader '{}'", shaderInputBindDesc.Name, uint32_t(shaderInputBindDesc.Type), _input.shader_name);
                        assert(false && "unsupported type");
                        break;
                }
            } break;
            case D3D_SIT_STRUCTURED:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::StructuredBuffer;  // todo struct size/stride?
                break;
            case D3D_SIT_UAV_RWSTRUCTURED:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWStructuredBuffer;
                break;
            case D3D_SIT_BYTEADDRESS:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::ByteAddressBuffer;
                break;
            case D3D_SIT_UAV_RWBYTEADDRESS:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RWByteAddressBuffer;
                break;
            case D3D_SIT_RTACCELERATIONSTRUCTURE:
                param_info.type = Moer::ReflectParamInfo::Dxil::EShaderVariableType::RaytracingAccelerationStructure;
                break;
            case D3D_SIT_UAV_APPEND_STRUCTURED:
            case D3D_SIT_UAV_CONSUME_STRUCTURED:
            case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            case D3D_SIT_TBUFFER:
            case D3D_SIT_UAV_FEEDBACKTEXTURE:
                LOG_WARNING("unsupported shader varibale '{}' type: '{}' in shader '{}'", shaderInputBindDesc.Name, uint32_t(shaderInputBindDesc.Type), _input.shader_name);
                assert(false && "unsupported type");
                break;
        }
    }

    _param_map.reflect_map.swap(reflect_map);
}
