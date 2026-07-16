// 负责调用 DXC 编译 Shader，并从 DXIL/SPIR-V 结果中提取资源绑定信息。
#include "DirectXShaderCompiler.h"
#include "config/ConfigManager.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "spirv.hpp"
#include "spirv_common.hpp"
#include <cassert>
#include <optional>
#include <variant>
#include <winerror.h>
#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif

#include "rhi/RHI.h"
#include <wrl/client.h>

#include "DXCUtils.h"
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdint.h>
#include <string>
#include <string_view>

#include "dxcapi.h"
#include "shader/ShaderCommon.h"
#include "spirv_cross.hpp"
#include <d3d12shader.h>

using Microsoft::WRL::ComPtr;
using ShaderParametersInfoMap   = Moer::Render::ShaderParametersInfoMap;
using ShaderCompilerEnvironment = Moer::Render::ShaderCompilerEnvironment;
using ShaderFileDependency      = Moer::Render::ShaderFileDependency;
using ReflectParamInfo          = Moer::Render::ReflectParamInfo;

// 包装默认 IDxcIncludeHandler，在每次 LoadSource 时记录被 include 文件的路径和时间戳。
// 用于构建 shader cache 的依赖列表，以便后续判断缓存是否因源文件变更而过期。
class TrackingIncludeHandler final : public IDxcIncludeHandler {
public:
    explicit TrackingIncludeHandler(IDxcIncludeHandler* wrapped) : m_wrapped(wrapped) {
        if (m_wrapped) m_wrapped->AddRef();
    }
    ~TrackingIncludeHandler() {
        if (m_wrapped) m_wrapped->Release();
    }

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override {
        HRESULT hr = m_wrapped->LoadSource(pFilename, ppIncludeSource);
        if (SUCCEEDED(hr) && pFilename) {
            std::filesystem::path fpath(pFilename);
            ShaderFileDependency dep;
            try {
                auto canonical = std::filesystem::canonical(fpath);
                dep.path      = canonical.generic_string();
                dep.timestamp = std::filesystem::last_write_time(canonical).time_since_epoch().count();
            } catch (...) {
                dep.path      = fpath.generic_string();
                dep.timestamp = 0;
            }
            m_included_files.push_back(std::move(dep));
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler)) {
            *ppvObject = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref_count; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = --m_ref_count;
        if (ref == 0) delete this;
        return ref;
    }

    Moer::Array<ShaderFileDependency> TakeIncludedFiles() { return std::move(m_included_files); }

private:
    IDxcIncludeHandler*               m_wrapped = nullptr;
    ULONG                             m_ref_count = 1;
    Moer::Array<ShaderFileDependency> m_included_files;
};

struct DXCompiler::Impl {
    Impl();
    ~Impl();

private:
    friend class DXCompiler;
    ComPtr<IDxcCompiler3>      compiler        = nullptr;
    ComPtr<IDxcLibrary>        library         = nullptr;
    ComPtr<IDxcUtils>          utils           = nullptr;
    ComPtr<IDxcIncludeHandler> include_handler = nullptr;

    void Compile(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output);

    void ReflectSPIRV(ComPtr<IDxcResult> result, ShaderParametersInfoMap& _param_map);
    void ReflectDXIL(
        ComPtr<IDxcResult>         result,
        const ShaderCompilerInput& _input,
        ShaderParametersInfoMap&   _param_map
    );
};

DXCompiler::Impl::Impl() {
    // Library 提供 include handler，Compiler 与 Utils 分别负责编译和结果处理。
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

DXCompiler& DXCompiler::GetInstance() {
    static DXCompiler s_compiler;
    return s_compiler;
}
DXCompiler::~DXCompiler() {
    MoerDelete(impl);
}

DXCompiler::DXCompiler() {
    impl = MoerNew(Impl)();
}

void DXCompiler::Impl::Compile(const ShaderCompilerInput& _input, ShaderCompilerOutput& _output) {
    const auto dxc_header_path =
        Moer::ConfigManager::GetInstance().GetWorkspacePath() / "3rdparty" / "dxc_2026_02_20" / "inc";
    const auto dxc_hlsl_header_path = dxc_header_path / "hlsl";

    auto push_back_error_message = [&_output](std::string message) {
        _output.errors.push_back(std::move(message));
        _output.b_succeeded = false;
    };

    auto add_dx_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(L"-DDXIL=1");

        arguments.push_back(DXC_ARG_DEBUG);
        arguments.push_back(L"-Qembed_debug");
    };

    auto add_vk_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(L"-spirv");
        arguments.push_back(L"-fspv-target-env=vulkan1.3");
        arguments.push_back(L"-fvk-use-dx-position-w");
        arguments.push_back(L"-fvk-use-dx-layout");
        arguments.push_back(L"-fvk-auto-shift-bindings");
        arguments.push_back(L"-fspv-preserve-interface");
        arguments.push_back(L"-DVULKAN=1");
        // 新版 DXC 的 SPIR-V ResourceDescriptorHeap 仍受上游问题 #7181 影响；
        // 在对应版本稳定前，DX 平台继续输出 DXIL，Vulkan 平台继续使用当前 SPIR-V 参数组合。
    };

    auto set_default_args = [add_dx_arg, add_vk_arg, dxc_header_path, dxc_hlsl_header_path](
                                Moer::Array<std::wstring>& arguments,
                                EShaderPlatform            _platform,
                                EShaderType                _type,
                                std::string_view           _entry_point
                            ) {
        arguments.push_back(L"-T");

        arguments.push_back(GetPlatform(_type, _platform));
        arguments.push_back(L"-E");
        arguments.emplace_back(std::wstring(_entry_point.begin(), _entry_point.end()));
        arguments.push_back(L"-I");
        arguments.push_back(Moer::ConfigManager::GetInstance().GetEngineShaderPath().generic_wstring());
        arguments.push_back(L"-I");
        arguments.push_back(Moer::ConfigManager::GetInstance().GetEngineShaderSharedPath().generic_wstring());
        // 允许 Shader 引用随仓库提供的 DXC 标准头，例如 cooperative_matrix.h。
        arguments.push_back(L"-I");
        arguments.push_back(dxc_header_path.generic_wstring());
        arguments.push_back(L"-I");
        arguments.push_back(dxc_hlsl_header_path.generic_wstring());
        if (_platform == SP_WIN_D3D_SM6)
            add_dx_arg(arguments);
        else if (_platform == SP_VULKAN_SM6)
            add_vk_arg(arguments);
    };
    auto add_debug_arg = [](Moer::Array<std::wstring>& arguments) {
        arguments.push_back(DXC_ARG_ALL_RESOURCES_BOUND);
        arguments.push_back(DXC_ARG_OPTIMIZATION_LEVEL3);
        arguments.push_back(DXC_ARG_DEBUG_NAME_FOR_BINARY);
    };

    auto add_define_arg = [](Moer::Array<std::wstring>&                          arguments,
                             const Moer::UnorderedMap<std::string, std::string>& _defines) {
        Moer::Array<DxcDefine> defines(_defines.size());

        for (const auto& define : _defines) {
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(
                define.first.begin(), define.first.end()
            );
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_second(
                define.second.begin(), define.second.end()
            );
            arguments.push_back(std::format(L"-D{}={}", temp_first, temp_second));
        }
    };

    auto add_compile_arg = [](Moer::Array<std::wstring>&                     arguments,
                              decltype(_input.environment.GetCompilerArgs()) _compile_args) {
        for (const auto& arg : _compile_args) {
            std::basic_string<wchar_t, std::char_traits<wchar_t>, m_defualt_allocator<wchar_t>> temp_first(
                arg.first.begin(), arg.first.end()
            );
            auto temp_second = ShaderCompilerEnvironment::GetVariantWStr(arg.second);
            arguments.push_back(std::format(L"-D{}={}", temp_first, temp_second));
        }
    };

    auto root_path = Moer::ConfigManager::GetInstance().GetEngineShaderPath();

    std::filesystem::path file_path;
    try {
        file_path = std::filesystem::canonical(root_path / _input.relative_source_file_path);
    } catch (const std::filesystem::filesystem_error& e) {
        LOG_ERROR(
            "Shader file not found: {}; Error info: {}",
            (root_path / _input.relative_source_file_path).string(),
            e.what()
        );
        // 此处没有足够上下文恢复，交由上层统一决定是否终止 Shader 编译流程。
        throw;
    }

    auto last_write_time = std::filesystem::last_write_time(file_path);

    Moer::Array<std::wstring> arguments = {file_path.generic_wstring().c_str()};

    set_default_args(
        arguments,
        (EShaderPlatform)_input.target_info.shader_platform,
        (EShaderType)_input.target_info.shader_type,
        _input.entry_point
    );
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
        // 编译接口要求完整源码缓冲区，因此这里先同步读取；异步化应由上层批处理编译负责。
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

        auto* tracking_handler = new TrackingIncludeHandler(include_handler.Get());
        HRESULT hres = compiler->Compile(
            &buffer,
            (LPCWSTR*)arguments_wchar.data(),
            (uint32_t)arguments_wchar.size(),
            tracking_handler,
            IID_PPV_ARGS(&result)
        );
        uint64_t result_hash[2] = {0, 0};

        if (SUCCEEDED(hres)) {
            result->GetStatus(&hres);
            ComPtr<IDxcBlob> p_hash = nullptr;
            result->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(&p_hash), nullptr);
            if (p_hash) {
                DxcShaderHash* shader_hash = (DxcShaderHash*)p_hash->GetBufferPointer();
                memcpy(result_hash, shader_hash->HashDigest, sizeof(result_hash));
            } else {
                // 某些 DXC 版本不返回 HASH 输出，此时对二进制两半分别计算稳定哈希。
                IDxcBlob* code;
                result->GetResult(&code);
                const uint8_t*   data = (uint8_t*)code->GetBufferPointer();
                uint32_t         size = code->GetBufferSize();
                std::string_view data_view((const char*)data, size / 2);

                result_hash[0] = GetHash(data_view);
                std::string_view data_view2((const char*)data + size / 2, size / 2);
                result_hash[1] = GetHash(data_view2);
            }
        }

        if (FAILED(hres) && (result)) {
            ComPtr<IDxcBlobEncoding> error_blob;
            hres = result->GetErrorBuffer(&error_blob);
            std::stringstream error_stream;
            if (SUCCEEDED(hres) && error_blob) {

                error_stream << "Shader compilation failed :\n"
                             << (const char*)error_blob->GetBufferPointer();
                push_back_error_message((const char*)error_blob->GetBufferPointer());
                tracking_handler->Release();
                return;
            }
        }

        if (_input.target_info.shader_platform == SP_VULKAN_SM6) {
            ReflectSPIRV(result, _output.parameter_map);
        } else {
            // D3D12 使用 DXIL 反射，Vulkan 使用 SPIR-V 反射。
            ReflectDXIL(result, _input, _output.parameter_map);
        }

        auto fill_success_data =
            [&_output, &last_write_time, &file_path, &result, &file_data, &_input, &result_hash,
             tracking_handler]() {
                IDxcBlob* code;
                result->GetResult(&code);
                const uint8_t* data = (uint8_t*)code->GetBufferPointer();
                uint32_t       size = code->GetBufferSize();
                _output.shader_code.resize(size);
                std::copy(data, data + size, _output.shader_code.begin());

                _output.b_succeeded      = true;
                _output.shader_name_hash = _input.shader_name_hash;
                _output.compiled_hash1              = result_hash[0];
                _output.compiled_hash2              = result_hash[1];
                _output.mutation_id                 = _input.mutation_id;
                _output.target_info                 = _input.target_info;
                _output.cached                      = false;
                _output.source_file_last_write_time = last_write_time.time_since_epoch().count();

                // 收集主文件 + include 文件的依赖信息
                _output.source_dependencies.clear();
                _output.source_dependencies.push_back(ShaderFileDependency{
                    .path      = file_path.generic_string(),
                    .timestamp = last_write_time.time_since_epoch().count(),
                });
                auto included = tracking_handler->TakeIncludedFiles();
                for (auto& dep : included) {
                    _output.source_dependencies.push_back(std::move(dep));
                }
            };

        fill_success_data();
        tracking_handler->Release();
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
        case SP_NumBits:
            break;
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
        case ST_Num:
            break;
        default:
            break;
    }
    return b_support_platform && b_support_shader_type;
}

void DXCompiler::Impl::ReflectSPIRV(ComPtr<IDxcResult> _result, ShaderParametersInfoMap& _param_map) {
    IDxcBlob* code;
    _result->GetResult(&code);
    const uint8_t* data = (uint8_t*)code->GetBufferPointer();
    uint32_t       size = code->GetBufferSize();

    ShaderReflectInfo reflect_info;

    Moer::UnorderedMap<std::string, ParameterInfo> param_map;
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
    if (!_param.has_value())   \
        _param = ReflectParamInfo::Bindless();

    Moer::UnorderedMap<std::string, ReflectParamInfo> reflect_map;

    {
        std::span<Moer::uint> spirv_code_span((Moer::uint*)data, size / sizeof(Moer::uint));
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

        auto handle_bdls_res_tex =
            [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
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
            };

        auto handle_res =
            [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
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

        auto handle_all_res =
            [&](spirv_cross::Resource& _res, EVulkanDescriptorType _desc_type, EShaderResourceType _srt) {
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
                handle_res(resource, VDT_UNIFORM_TEXEL_BUFFER, EShaderResourceType::SRT_SRV);
            } else {
                handle_all_res(resource, VDT_SAMPLED_IMAGE, EShaderResourceType::SRT_SRV);
            }
        }

        for (auto& resource : resources.separate_samplers) {
            handle_all_res(resource, VDT_SAMPLER, EShaderResourceType::SRT_SAMPLER);
        }

        for (auto& resource : resources.storage_buffers) {
            spirv_cross::Bitset buffer_flags = comp.get_buffer_block_flags(resource.id);
            handle_all_res(
                resource,
                VDT_STORAGE_BUFFER,
                buffer_flags.get(spv::DecorationNonWritable) ? EShaderResourceType::SRT_SRV :
                                                               EShaderResourceType::SRT_UAV
            );
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

        for (auto& resource : resources.acceleration_structures) {
            handle_all_res(resource, VDT_ACCELERATION_STRUCTURE, SRT_SRV);
        }
    }
#undef SetZeroIfEmpty

#undef GET_RESOURCE_DEFAULT_INFOS

    _param_map.reflect_map.swap(reflect_map);
}

#define DX_CHECK_HRESULT(hr)                                         \
    do {                                                             \
        HRESULT _hr = (hr);                                          \
        if (_hr < 0) {                                               \
            LOG_CRITICAL("ERROR: hresult={:#x}", (unsigned int)_hr); \
            std::terminate();                                        \
        }                                                            \
    } while (0)

void DXCompiler::Impl::ReflectDXIL(
    ComPtr<IDxcResult>         result,
    const ShaderCompilerInput& _input,
    ShaderParametersInfoMap&   _param_map
) {
    using Moer::uint;

    // DXC 将反射数据作为独立输出返回，不能直接复用编译后的 Shader Blob。
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

    Moer::UnorderedMap<std::string, ReflectParamInfo> reflect_map;

    constexpr std::string_view bdls_array_suffix = "_array_114514_bdls";
    auto                       is_bdls_array     = [&](const std::string& _name) {
        return _name.ends_with(bdls_array_suffix);
    };

    for (int i = 0; i < shaderDesc.BoundResources; ++i) {
        D3D12_SHADER_INPUT_BIND_DESC shaderInputBindDesc{};
        DX_CHECK_HRESULT(shaderReflection->GetResourceBindingDesc(i, &shaderInputBindDesc));

        std::string name = shaderInputBindDesc.Name;
        if (is_bdls_array(name)) {
            name = ReflectParamInfo::bdls_name; // Bindless 数组统一映射到内部保留名称。
        }
        auto& param_info = reflect_map[name].dxil;
        param_info.slot  = shaderInputBindDesc.BindPoint;
        param_info.space = shaderInputBindDesc.Space;
        param_info.count = shaderInputBindDesc.BindCount;

        switch (shaderInputBindDesc.Type) {
            case D3D_SIT_CBUFFER: {
                // 当前统一按 ConstantBuffer 反射；如需 RootConstant，应在参数模型层单独设计。
                param_info.type = uint(ED3D12ShaderVariableType::ConstantBuffer);
                auto* cb        = shaderReflection->GetConstantBufferByName(shaderInputBindDesc.Name);
                assert(cb);
                D3D12_SHADER_BUFFER_DESC desc{};
                DX_CHECK_HRESULT(cb->GetDesc(&desc));
                param_info.byte_size = desc.Size;
                assert(shaderInputBindDesc.BindCount == 1); // ? array of constant buffer
            } break;
            case D3D_SIT_TEXTURE: {
                const auto dim = shaderInputBindDesc.Dimension;
                switch (dim) {
                    case D3D_SRV_DIMENSION_BUFFER:
                        param_info.type = uint(ED3D12ShaderVariableType::TypedBuffer);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1D:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture1D);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture1DArray);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture2D);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture2DArray);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DMS:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture2DMS);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture2DMSArray);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        param_info.type = uint(ED3D12ShaderVariableType::Texture3D);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURECUBE:
                        param_info.type = uint(ED3D12ShaderVariableType::TextureCube);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURECUBEARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::TextureCubeArray);
                        break;
                    default:
                        LOG_WARNING(
                            "unsupported shader varibale '{}' dimension: '{}' in shader '{}'",
                            shaderInputBindDesc.Name,
                            uint(shaderInputBindDesc.Type),
                            _input.shader_name
                        );
                        assert(false && "unsupported type");
                        break;
                }
            } break;
            case D3D_SIT_SAMPLER:
                param_info.type = uint(ED3D12ShaderVariableType::Sampler);
                break;
            case D3D_SIT_UAV_RWTYPED: {
                // Shader Model 6.7 才引入可写多重采样纹理；当前枚举仅覆盖现有编译目标使用的类型。
                const auto dim = shaderInputBindDesc.Dimension;
                switch (dim) {
                    case D3D_SRV_DIMENSION_BUFFER:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTypedBuffer);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1D:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTexture1D);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTexture1DArray);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2D:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTexture2D);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTexture2DArray);
                        break;
                    case D3D_SRV_DIMENSION_TEXTURE3D:
                        param_info.type = uint(ED3D12ShaderVariableType::RWTexture3D);
                        break;
                    default:
                        LOG_WARNING(
                            "unsupported shader varibale '{}' rw dimension: '{}' in shader '{}'",
                            shaderInputBindDesc.Name,
                            uint(shaderInputBindDesc.Type),
                            _input.shader_name
                        );
                        assert(false && "unsupported type");
                        break;
                }
            } break;
            case D3D_SIT_STRUCTURED:
                param_info.type =
                    uint(ED3D12ShaderVariableType::StructuredBuffer); // todo 32_tstruct size/stride)?
                break;
            case D3D_SIT_UAV_RWSTRUCTURED:
                param_info.type = uint(ED3D12ShaderVariableType::RWStructuredBuffer);
                break;
            case D3D_SIT_BYTEADDRESS:
                param_info.type = uint(ED3D12ShaderVariableType::ByteAddressBuffer);
                break;
            case D3D_SIT_UAV_RWBYTEADDRESS:
                param_info.type = uint(ED3D12ShaderVariableType::RWByteAddressBuffer);
                break;
            case D3D_SIT_RTACCELERATIONSTRUCTURE:
                param_info.type = uint(ED3D12ShaderVariableType::RaytracingAccelerationStructure);
                break;
            case D3D_SIT_UAV_APPEND_STRUCTURED:
            case D3D_SIT_UAV_CONSUME_STRUCTURED:
            case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            case D3D_SIT_TBUFFER:
            case D3D_SIT_UAV_FEEDBACKTEXTURE:
                LOG_WARNING(
                    "unsupported shader varibale '{}' type: '{}' in shader '{}'",
                    shaderInputBindDesc.Name,
                    uint(shaderInputBindDesc.Type),
                    _input.shader_name
                );
                assert(false && "unsupported type");
                break;
        }
    }

    _param_map.reflect_map.swap(reflect_map);
}
