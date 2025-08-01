#include "D3D12Device.h"

#include <string_view>

#include "log/LogSystem.h"
#include "rhi/RHIResourceInitilizer.h"

#include "Core.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/ThreadManager.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <variant>
#include <config.h>
#include <platform/Platform.h>
#include "../vulkan/RHICmdReorderer.h"

namespace Moer::Render {

    // todo? logging, add a d3d12 sink? , with specialize output prefix e.g. [D3D12]

    //https://stackoverflow.com/questions/16190078/how-to-atomically-update-a-maximum-value
    // (look forward to c++26 fetch_max
    template<typename T>
    void AtomicMaximum(std::atomic<T>& maximum_value, T const& value) noexcept {
        T prev_value = maximum_value;
        while (prev_value < value &&
               !maximum_value.compare_exchange_weak(prev_value, value)) {
            //; // no-op
            std::this_thread::yield();
        }
    }

    static void ReportLiveObjects() {
        ComPtr<IDXGIDebug1> dxgi_debug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgi_debug.ReleaseAndGetAddressOf())))) {
            dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }

    Moer::Render::D3D12Device::D3D12Device(const D3D12RHIConfig&& _config) : config(_config) {

        UINT dxgiFactoryFlags = 0;

        std::atexit(ReportLiveObjects);

#if DX_DEBUG

        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_interface.ReleaseAndGetAddressOf())))) {
            debug_interface->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }

        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(dxgi_info_queue.ReleaseAndGetAddressOf())))) {
            dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, true);
            dxgi_info_queue->SetBreakOnSeverity(DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, true);
        }

#endif

        DX_CHECK_HRESULT(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));

        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory, hardwareAdapter.ReleaseAndGetAddressOf(), true);

        DX_CHECK_HRESULT(D3D12CreateDevice(
            hardwareAdapter.Get(),
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));
        DX_CHECK_HRESULT(hardwareAdapter.As(&adapter));
        hardwareAdapter = nullptr;

#if DX_DEBUG

        DX_CHECK_HRESULT(device.As(&debug_info_queue));

        {
            // from directx-graphics-sample

            // Suppress whole categories of messages
            //D3D12_MESSAGE_CATEGORY Categories[] = {};

            // Suppress messages based on their severity level
            D3D12_MESSAGE_SEVERITY Severities[]{D3D12_MESSAGE_SEVERITY_INFO};

            // Suppress individual messages by their ID
            D3D12_MESSAGE_ID DenyIds[] =
                {
                    // This occurs when there are uninitialized descriptors in a descriptor table, even when a
                    // shader does not access the missing descriptors.  I find this is common when switching
                    // shader permutations and not wanting to change much code to reorder resources.
                    D3D12_MESSAGE_ID_INVALID_DESCRIPTOR_HANDLE,

                    // Triggered when a shader does not export all color components of a render target, such as
                    // when only writing RGB to an R10G10B10A2 buffer, ignoring alpha.
                    D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_PS_OUTPUT_RT_OUTPUT_MISMATCH,

                    // This occurs when a descriptor table is unbound even when a shader does not access the missing
                    // descriptors.  This is common with a root signature shared between disparate shaders that
                    // don't all need the same types of resources.
                    D3D12_MESSAGE_ID_COMMAND_LIST_DESCRIPTOR_TABLE_NOT_SET,

                    // RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS
                    (D3D12_MESSAGE_ID)1008,
                };

            D3D12_INFO_QUEUE_FILTER NewFilter = {};
            //NewFilter.DenyList.NumCategories = _countof(Categories);
            //NewFilter.DenyList.pCategoryList = Categories;
            NewFilter.DenyList.NumSeverities = _countof(Severities);
            NewFilter.DenyList.pSeverityList = Severities;
            NewFilter.DenyList.NumIDs        = _countof(DenyIds);
            NewFilter.DenyList.pIDList       = DenyIds;

            debug_info_queue->PushStorageFilter(&NewFilter);
        }

        debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
        debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_INFO, false);
        debug_info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_MESSAGE, false);

        if (SUCCEEDED(debug_interface.As(&debug_info_queue1))) {// always failed because of not support ???
            // NOTE seems wee need at least 'Windows 10 Release Preview build 20236' to use this feature, maybe win11? sunqi 22631 test fail
            //      https://devblogs.microsoft.com/directx/d3d12-debug-layer-message-callback/
            D3D12MessageFunc debug_log = +[](D3D12_MESSAGE_CATEGORY Category,
                                             D3D12_MESSAGE_SEVERITY Severity,
                                             D3D12_MESSAGE_ID       ID,
                                             LPCSTR                 pDescription,
                                             void*                  pContext) {
                LOG_ERROR("D3D12 VALIDATION: {},{},{},{}", uint(Category), uint(Severity), uint(ID), pDescription);
            };

            DX_CHECK_HRESULT(debug_info_queue1->RegisterMessageCallback(debug_log, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &debug_log_callback_cookie));
            ASSERT(debug_log_callback_cookie != 0);
        } else {
            LOG_INFO("ID3D12InfoQueue1 not available, no custom validation messages will be shown");
        }
#endif

        feature_supports.Init(device.Get());
        ASSERT(feature_supports.EnhancedBarriersSupported());
        ASSERT(feature_supports.ResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_3);// ResourceDescriptorHeap[index]
        ASSERT(feature_supports.HighestShaderModel() >= D3D_SHADER_MODEL_6_6);          // ResourceDescriptorHeap[index]
        ASSERT(feature_supports.RaytracingTier() >= D3D12_RAYTRACING_TIER_1_1);         // 1.0 for basic dxr, 1.1 for rayquery
        ASSERT(feature_supports.HighestRootSignatureVersion() >= D3D_ROOT_SIGNATURE_VERSION_1_1);

        gpu_global_allocator = MakeUnique<D3D12GpuGlobalAllocator>(this);
        gfx_queue            = MakeUnique<D3D12GraphicsCommandQueue>(this, EQueueType::Graphics);
        csu_heap_cpu         = MakeUnique<D3D12CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, feature_supports.MaxViewDescriptorHeapSize());// ~1e6
        rtv_heap_cpu         = MakeUnique<D3D12CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256);                                                 // just some small number, should be enough
        dsv_heap_cpu         = MakeUnique<D3D12CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256);
        sampler_heap_cpu     = MakeUnique<D3D12CpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, feature_supports.MaxSamplerDescriptorHeapSizeWithStaticSamplers());         // ~2048
        csu_heap_gpu         = MakeUnique<D3D12GpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, feature_supports.MaxViewDescriptorHeapSize() / 2, 4096);                // ~1e6
        sampler_heap_gpu     = MakeUnique<D3D12GpuDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, feature_supports.MaxSamplerDescriptorHeapSizeWithStaticSamplers() / 2, 256);// ~2048
    }

    D3D12Device::~D3D12Device() {
#if DX_DEBUG
        if (debug_info_queue1) {
            DX_CHECK_HRESULT(debug_info_queue1->UnregisterMessageCallback(debug_log_callback_cookie));
        }
#endif
    }

    PipelineHandle D3D12Device::CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) {
        return PipelineHandle();
    }

    static bool CheckReflectionTypeMatch(const ReflectParamInfo::Dxil& resource_info, const ShaderArgCppInfo& arg_info) {
        if (arg_info.type == EShaderArgType::SDA_BindlessArray)// !just a specical case.. not mean real bindless.  'StructuredBuffer<uint> g__array_114514_bdls'
            return resource_info.type == ED3D12ShaderVariableType::StructuredBuffer;
        if (arg_info.type == EShaderArgType::SDA_Constant) return IsShaderVarRootConstant(resource_info);
        //if (arg_info.type == EShaderArgType::SDA_ConstantBuffer) return resource_info.IsConstantBuffer(); // seems 'SDA_ConstantBuffer' not actually used...
        if (arg_info.type == EShaderArgType::SDA_Buffer) return IsShaderVarCommonBuffer(resource_info) || IsShaderVarConstantBuffer(resource_info);
        if (arg_info.type == EShaderArgType::SDA_Texture) return IsShaderVarTexture(resource_info);
        if (arg_info.type == EShaderArgType::SDA_Sampler) return IsShaderVarSampler(resource_info);
        if (arg_info.type == EShaderArgType::SDA_TLAS) return IsShaderVarRaytracingAccelerationStructure(resource_info);
        return false;
    };

    uint D3D12PipelineState::PipelineLayout::RootDescriptorTable::GetTotalBindCount() const {
        uint cnt = 0;
        for (const auto& e : entries) {
            ASSERT(e.bind_count > 0);// not consider bindless now
            cnt += e.bind_count;
        }
        return cnt;
    }

    void D3D12PipelineState::PipelineLayout::Add(uint8 _idx_in_cpp_args, const ShaderArgCppInfo& _arg_cpp_info, const ReflectParamInfo::Dxil& _resource_info, bool _b_special_bindless) {
        if (_b_special_bindless) {
            the_bindless_array.emplace(_idx_in_cpp_args, uint8(-1), uint8(_resource_info.slot), uint8(_resource_info.space));
        } else if (IsShaderVarRootConstant(_resource_info)) {
            root_constant.emplace(_idx_in_cpp_args, uint8(-1), _resource_info.byte_size, uint8(_resource_info.slot), uint8(_resource_info.space));
        } else {
            ASSERT(!_resource_info.IsBindless());// maybe not consider user defined bindless. just builtin bindless array.
            const uint bind_count = _resource_info.IsBindless() ? 0 : _resource_info.count;
            if (!_resource_info.IsBindless()) {
                ASSERT(_resource_info.count == _arg_cpp_info.array_size);
            }
            if (IsShaderVarConstantBuffer(_resource_info)) {
                if (!root_cbvs) {
                    root_cbvs.emplace();
                }
                root_cbvs->emplace_back(_idx_in_cpp_args, uint8(-1), uint8(_resource_info.slot), uint8(_resource_info.space));
            } else if (IsShaderVarSrv(_resource_info)) {
                if (!srv_table) {
                    srv_table.emplace();
                }
                srv_table->entries.emplace_back(_idx_in_cpp_args, uint8(srv_table->entries.size()), uint8(bind_count), uint8(_resource_info.slot), uint8(_resource_info.space));
            } else if (IsShaderVarUav(_resource_info)) {
                if (!uav_table) {
                    uav_table.emplace();
                }
                uav_table->entries.emplace_back(_idx_in_cpp_args, uint8(uav_table->entries.size()), uint8(bind_count), uint8(_resource_info.slot), uint8(_resource_info.space));
            } else if (IsShaderVarSampler(_resource_info)) {
                if (!sampler_table) {
                    sampler_table.emplace();
                }
                sampler_table->entries.emplace_back(_idx_in_cpp_args, uint8(sampler_table->entries.size()), uint8(bind_count), uint8(_resource_info.slot), uint8(_resource_info.space));
            }
        }
    }

    void D3D12PipelineState::BuildRootSignature(const PipelineLayout& _layout) {
        layout = _layout;

        uint                             required_dword = 0;// due to root sig limit, max 64 dword, 256 bytes
        Array<CD3DX12_ROOT_PARAMETER1>   root_parameters;
        Array<CD3DX12_DESCRIPTOR_RANGE1> descriptor_ranges;
        uint                             num_root_parameters   = 0;
        uint                             num_descriptor_ranges = 0;

        {
            // first pass check size
            if (layout.root_constant) {
                required_dword += layout.root_constant->byte_size / 4;
                layout.root_constant->idx_in_root_sig = num_root_parameters++;
            }
            if (layout.the_bindless_array) {
                required_dword += 2;
                layout.the_bindless_array->idx_in_root_sig = num_root_parameters++;
            }
            if (layout.root_cbvs) {
                for (auto& cbv : *layout.root_cbvs) {
                    required_dword += 2;
                    cbv.idx_in_root_sig = num_root_parameters++;
                }
            }
            if (layout.srv_table) {
                required_dword++;
                layout.srv_table->idx_in_root_sig = num_root_parameters++;
                num_descriptor_ranges += layout.srv_table->entries.size();
            }
            if (layout.uav_table) {
                required_dword++;
                layout.uav_table->idx_in_root_sig = num_root_parameters++;
                num_descriptor_ranges += layout.uav_table->entries.size();
            }
            if (layout.sampler_table) {
                required_dword++;
                layout.sampler_table->idx_in_root_sig = num_root_parameters++;
                num_descriptor_ranges += layout.sampler_table->entries.size();
            }
        }

        root_parameters.resize(num_root_parameters);
        descriptor_ranges.resize(num_descriptor_ranges);
        ASSERT(required_dword <= 64);

        {
            // ref UE D3D12RootSignature.cpp
            static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS kSrvDescriptorRangeFlags     = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS kCbvDescriptorRangeFlags     = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS kUavDescriptorRangeFlags     = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            static constexpr D3D12_DESCRIPTOR_RANGE_FLAGS kSamplerDescriptorRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
            static constexpr D3D12_ROOT_DESCRIPTOR_FLAGS  kCbvRootDescriptorFlags      = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
            // UE: D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;// We always set the data in an upload heap before calling Set*RootConstantBufferView.

            // todo Visibility?

            // second pass fill data
            if (layout.root_constant) {
                root_parameters[layout.root_constant->idx_in_root_sig].InitAsConstants(layout.root_constant->byte_size / 4, layout.root_constant->slot, layout.root_constant->space, D3D12_SHADER_VISIBILITY_ALL);
            }
            if (layout.the_bindless_array) {
                root_parameters[layout.the_bindless_array->idx_in_root_sig].InitAsShaderResourceView(layout.the_bindless_array->slot, layout.the_bindless_array->space, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, D3D12_SHADER_VISIBILITY_ALL);
            }
            if (layout.root_cbvs) {
                for (const auto& cbv : *layout.root_cbvs) {
                    root_parameters[cbv.idx_in_root_sig].InitAsConstantBufferView(cbv.slot, cbv.space, kCbvRootDescriptorFlags, D3D12_SHADER_VISIBILITY_ALL);
                }
            }
            uint range_offset = 0;
            if (layout.srv_table) {
                for (const auto& e : layout.srv_table->entries) {
                    descriptor_ranges[range_offset + e.idx_in_table].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, e.bind_count, e.slot, e.space, kSrvDescriptorRangeFlags);// one table can have different type ranges. we now do like this for simplicity
                }
                root_parameters[layout.srv_table->idx_in_root_sig].InitAsDescriptorTable(layout.srv_table->entries.size(), descriptor_ranges.data() + range_offset, D3D12_SHADER_VISIBILITY_ALL);
                range_offset += layout.srv_table->entries.size();
            }
            if (layout.uav_table) {
                for (const auto& e : layout.uav_table->entries) {
                    descriptor_ranges[range_offset + e.idx_in_table].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, e.bind_count, e.slot, e.space, kUavDescriptorRangeFlags);
                }
                root_parameters[layout.uav_table->idx_in_root_sig].InitAsDescriptorTable(layout.uav_table->entries.size(), descriptor_ranges.data() + range_offset, D3D12_SHADER_VISIBILITY_ALL);
                range_offset += layout.uav_table->entries.size();
            }
            if (layout.sampler_table) {
                for (const auto& e : layout.sampler_table->entries) {
                    descriptor_ranges[range_offset + e.idx_in_table].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, e.bind_count, e.slot, e.space, kSamplerDescriptorRangeFlags);
                }
                root_parameters[layout.sampler_table->idx_in_root_sig].InitAsDescriptorTable(layout.sampler_table->entries.size(), descriptor_ranges.data() + range_offset, D3D12_SHADER_VISIBILITY_ALL);
                range_offset += layout.sampler_table->entries.size();
            }
        }

        D3D12_ROOT_SIGNATURE_FLAGS root_sig_flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC root_sig_desc;
        root_sig_desc.Init_1_1(root_parameters.size(), root_parameters.data(), 0, nullptr, root_sig_flags);//todo static samplers

        D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data{.HighestVersion = device->GetFeatureSupport().HighestRootSignatureVersion()};
        ComPtr<ID3DBlob>                  signature_blob;
        ComPtr<ID3DBlob>                  error;
        DX_CHECK_HRESULT(D3DX12SerializeVersionedRootSignature(&root_sig_desc, feature_data.HighestVersion, signature_blob.GetAddressOf(), error.GetAddressOf()));
        DX_CHECK_HRESULT(device->Native()->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(root_signature.GetAddressOf())));
    }

    PipelineHandle D3D12Device::CreatePipeline(PipelineShaderInfo&& _shaders) {
        ASSERT(std::holds_alternative<ShaderCs>(_shaders.shader_group));

        SingleShaderInfo& cs_info         = std::get<ShaderCs>(_shaders.shader_group).cs;
        const auto&       shader_bytecode = cs_info.shader_data;

        // TODO shader reflection-> pipeline reflection

        auto& reflect_map = cs_info.shader_param_map->reflect_map;

        Array<ParamInfoFlags> binding_infos(_shaders.layout_hash.size());

        D3D12PipelineState::PipelineLayout layout;

        ASSERT(_shaders.layout_hash.size() == _shaders.arg_cpp_info.size());
        const auto& resource_names = _shaders.layout_hash;// (not hash, but resource names
        for (size_t i = 0; i < resource_names.size(); ++i) {
            const ShaderArgCppInfo& arg_info = _shaders.arg_cpp_info[i];

            std::string name_internal(resource_names[i]);
            bool        b_special_bindless = false;
            if (arg_info.type == EShaderArgType::SDA_BindlessArray) {
                name_internal      = ReflectParamInfo::bdls_name;// !
                b_special_bindless = true;
            }

            auto reflect_map_iter = reflect_map.find(name_internal.data());
            if (reflect_map_iter == reflect_map.end()) {
                LOG_WARNING("provided pipeline arg '{}' not found in shader '{}'", resource_names[i], cs_info.name);
                continue;
            }

            auto& resource_info = reflect_map_iter->second.dxil;
            if (arg_info.type == EShaderArgType::SDA_Constant) {
                ASSERT(IsShaderVarConstantBuffer(resource_info));
                resource_info.type = uint(ED3D12ShaderVariableType::RootConstant);
            }

            ASSERT2(CheckReflectionTypeMatch(resource_info, arg_info), std::format("reflection mismatch for resource {} in shader {}", resource_names[i], cs_info.name));

            binding_infos[i].pipeline_flags = uint64(D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            binding_infos[i].state_flags    = uint64(resource_info.type);

            layout.Add(uint8(i), arg_info, resource_info, b_special_bindless);
        }

        D3D12PipelineState* pso = MoerNew(D3D12PipelineState)(this, D3D12PipelineState::Compute);
        pso->BuildRootSignature(layout);

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature                  = pso->root_signature.Get();
        pso_desc.CS.pShaderBytecode              = shader_bytecode.data();
        pso_desc.CS.BytecodeLength               = shader_bytecode.size();
        pso_desc.NodeMask                        = 0;
        pso_desc.CachedPSO.pCachedBlob           = nullptr;
        pso_desc.CachedPSO.CachedBlobSizeInBytes = 0;
        pso_desc.Flags                           = D3D12_PIPELINE_STATE_FLAG_NONE;
        ComPtr<ID3D12PipelineState> pipelineState;
        DX_CHECK_HRESULT(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(pso->pipeline_state.ReleaseAndGetAddressOf())));

        return PipelineHandle{.handle = reinterpret_cast<uint64>(pso), .binding_infos = std::move(binding_infos)};
    }

    TextureRef D3D12Device::CreateTexture(std::string_view _name, ETextureDimension _dimension, Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint _array_size) {
        bool        b_depth = uint(ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT & _usage) != 0;
        TextureInfo info{
            _dimension,
            _usage,
            _format,
            b_depth ? EClearAttachment::DEPTH_STENCIL : EClearAttachment::COLOR,
            _size,
            uint8(_mip_cnt),
            uint8(_dimension == ETextureDimension::TEX_CUBE ? 6 : _array_size),
            1};// TODO ? msaa tex
        info.aspect_flags = b_depth ? ETextureAspectFlags::DEPTH_SLICE : ETextureAspectFlags::COLOR;
        info.debug_name   = _name;
        return TextureRef{MoerNew(D3D12Texture)(this, info)};
    }

    BufferRef D3D12Device::CreateBuffer(std::string_view _name, uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage, EPixelFormat _format) {
        BufferInfo info{_element_cnt, _byte_stride, _usage, _format};
        // _format unused now
        auto* buffer = MoerNew(D3D12Buffer)(this, info);
        buffer->SetName(_name);
        return BufferRef{buffer};
    }

    BindlessArrayRef D3D12Device::CreateBindlessArray(uint _max_size) {
        return nullptr;
    }

    FenceRef D3D12Device::CreateFence() {
        return FenceRef{MoerNew(D3D12Fence)(this)};
    }

    RaytracingGeometryRef D3D12Device::CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) {
        return nullptr;
    }

    RaytracingSceneRef D3D12Device::CreateRaytracingScene() {
        return nullptr;
    }

    CommandQueue& D3D12Device::GetCommandQueue(EQueueType _type) {
        if (_type == EQueueType::Graphics) return *gfx_queue;
        // TODO: 在此处插入 return 语句
        struct DummyCommandQueue : CommandQueue {
            void                Test();
            virtual void        Wait(WaitEvent _event) {};
            virtual WaitEvent   Execute(CmdSubmit&& _submit) { return {}; };
            virtual void        Present(SwapchainRef _swapchain, TextureView _target) {};
            virtual void        Sync() {};
            virtual ProfileData GetProfilerEntry() { return {}; }
        };
        static DummyCommandQueue queue;
        return queue;
    }

    CopyQueue& D3D12Device::GetCopyQueue() {
        // TODO: 在此处插入 return 语句
        struct DummyCopyQueue : CopyQueue {
            IOWaitEvt         Execute(IOSubmission&& _submit) { return {}; }
            virtual IOWaitEvt Execute(CmdSubmit&& _submit) { return {}; }
            virtual IOWaitEvt Execute(IOQueueSubmission&& _submit) { return {}; }
            virtual void      CopyFrom(BufferView _src, BufferView _dst) {};
            virtual void      CopyFrom(TextureView _src, TextureView _dst) {};
            virtual void      CopyFrom(TextureView _src, BufferView _dst) {};
            virtual void      CopyFrom(BufferView _src, TextureView _dst) {};
            virtual void      CopyFrom(std::span<byte> _data, BufferView _dst) {};
            virtual void      CopyFrom(std::span<byte> _data, TextureView _dst) {};

            virtual FenceRef GetFenceHandle() { return nullptr; };
            virtual void     Sync(uint64 _timeline) {};
        };
        static DummyCopyQueue queue;
        return queue;
    }

    SwapchainRef D3D12Device::CreateSwapchain(const SwapchainCreateInfo& _info) {
        return SwapchainRef();
    }

    void Moer::Render::D3D12Device::PostInit() {
    }

    void D3D12Device::GetHardwareAdapter(
        ComPtr<IDXGIFactory4> pFactory,
        IDXGIAdapter1**       ppAdapter,
        bool                  requestHighPerformanceAdapter) {
        *ppAdapter = nullptr;

        ComPtr<IDXGIAdapter1> adapter;

        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(pFactory.As(&factory6))) {
            for (
                UINT adapterIndex = 0;
                SUCCEEDED(factory6->EnumAdapterByGpuPreference(
                    adapterIndex,
                    requestHighPerformanceAdapter == true ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                    IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())));
                ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }
                if (desc.VendorId != 0x10de) {
                    continue;
                }
                // want nvidia

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    OutputDebugStringW(desc.Description);
                    break;
                }
            }
        }

        if (adapter.Get() == nullptr) {
            for (UINT adapterIndex = 0; SUCCEEDED(pFactory->EnumAdapters1(adapterIndex, adapter.ReleaseAndGetAddressOf())); ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);

                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    // Don't select the Basic Render Driver adapter.
                    // If you want a software adapter, pass in "/warp" on the command line.
                    continue;
                }

                // Check to see whether the adapter supports Direct3D 12, but don't create the
                // actual device yet.
                // FIXME?? here give Poco::NotFoundException
                if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    break;
                }
            }
        }

        *ppAdapter = adapter.Detach();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::PushCsuDescriptor(std::span<const DescriptorIndex> _index_in_cpu_heap) {
        const uint            count = _index_in_cpu_heap.size();
        const DescriptorIndex start = csu_heap_gpu->Allocate(count);
        for (uint i = 0; i < count; ++i) {
            device->CopyDescriptorsSimple(1, csu_heap_gpu->GetOffsetHandleCpu({start.index + i}), csu_heap_cpu->GetOffsetHandleCpu(_index_in_cpu_heap[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return csu_heap_gpu->GetOffsetHandleGpu(start);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::PushSamplerDescriptor(std::span<const DescriptorIndex> _index_in_cpu_heap) {
        const uint            count = _index_in_cpu_heap.size();
        const DescriptorIndex start = sampler_heap_gpu->Allocate(count);
        for (uint i = 0; i < count; ++i) {
            device->CopyDescriptorsSimple(1, sampler_heap_gpu->GetOffsetHandleCpu({start.index + i}), sampler_heap_cpu->GetOffsetHandleCpu(_index_in_cpu_heap[i]), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        return sampler_heap_gpu->GetOffsetHandleGpu(start);
    }

    D3D12Swapchain::D3D12Swapchain(D3D12Device& device, const SwapchainCreateInfo& info) : device(device) {
        Recreate(info);
    }

    void D3D12Swapchain::Recreate(const SwapchainCreateInfo& _info) {
        FATAL("not implemented");
        //if (width == _info.size.width && height == _info.size.height && window_handle == _info.window_handle && backbuffers.size() == _info.back_buffer_sz) {
        //    return;
        //}
        //bool b_recreate = false;// false when create for first time
        //if (window_handle != 0) {
        //    ASSERT(window_handle == _info.window_handle);
        //    b_recreate = true;
        //}
        //window_handle = _info.window_handle;
        //width         = _info.size.width;
        //height        = _info.size.height;
        //backbuffers.resize(_info.back_buffer_sz);

        //// note to sync before recreate
        //DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
        //ZeroMemory(&swapChainDesc, sizeof(swapChainDesc));
        //swapChainDesc.BufferCount        = backbuffers.size();
        //swapChainDesc.Width              = width;
        //swapChainDesc.Height             = height;
        //swapChainDesc.Format             = desc.format;// TODO info.format
        //swapChainDesc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        //swapChainDesc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        //swapChainDesc.SampleDesc.Count   = 1;
        //swapChainDesc.SampleDesc.Quality = 0;
        //swapChainDesc.Flags              = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;  // consider vsync?

        //ComPtr<IDXGISwapChain1> swapchain1;
        ////DX_CHECK_HRESULT(device->GetFactory()->CreateSwapChainForHwnd(
        ////    m_device->GetGraphicsCommandContext().GetQueue(),// Swap chain needs the queue so that it can force a flush on it.
        ////    windowHandle,
        ////    &swapChainDesc,
        ////    nullptr,
        ////    nullptr,
        ////    swapChain.put()));
        //DX_CHECK_HRESULT(swapchain1.As(&swapchain));
        //swapchain1 = nullptr;
        //frame_index = swapchain->GetCurrentBackBufferIndex();

        //DX_CHECK_HRESULT(device.GetFactory()->MakeWindowAssociation(reinterpret_cast<HWND>(window_handle), DXGI_MWA_NO_ALT_ENTER)); // maybe allow alt+enter to switch to full screen

        //CreateSizeDependentResources();
    }

    void D3D12Swapchain::Sync() {
        FATAL("not implemented");
    }

    void D3D12Swapchain::Present() {
    }

    void D3D12Swapchain::CreateSizeDependentResources() {
        // Create a RTV for each frame.
        //for (UINT n = 0; n < FrameLatency; n++) {
        //    //m_backbufferRTVs[n] = m_device->GetRtvHeap().Allocate();
        //    //ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(m_backbuffers[n].put())));
        //    //m_device->GetDevice()->CreateRenderTargetView(m_backbuffers[n].get(), nullptr, m_device->GetRtvHeap().GetCpuHandleCpuHeap(m_backbufferRTVs[n]));
        //    //SetObjectDebugName(m_backbuffers[n].get(), "backbuffer", n);

        //}
    }

    D3D12Buffer::D3D12Buffer(D3D12Device* _device, const BufferInfo& _info) : Buffer(_info), D3D12DeviceChild(_device) {
        DASSERT(false == EnumHasAnyFlag(_info.usage, EBufferUsageFlags::CPU_VISIBLE));
        auto* allocator = _device->GetGpuGlobalAllocator();

        allocation = allocator->AllocateBufferHeap("default-buffer-name", GetByteSize(), D3D12_HEAP_TYPE_DEFAULT);

        D3D12_RESOURCE_DESC1 resourceDesc = CD3DX12_RESOURCE_DESC1::Buffer(GetByteSize());
        if (EnumHasAnyFlag(_info.usage, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH)) {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        if (EnumHasAnyFlag(_info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE | EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT)) {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
        }

        DX_CHECK_HRESULT(_device->Native()->CreatePlacedResource2(
            allocation.alloc->GetHeap(),
            allocation.alloc->GetOffset(),
            &resourceDesc,
            D3D12_BARRIER_LAYOUT_UNDEFINED,// buffer resources have no layout
            nullptr,                       // no clear value
            0,                             // omit castable format
            nullptr,
            IID_PPV_ARGS(&allocation.resource)));
    }

    D3D12Buffer::D3D12Buffer(D3D12Device* _device, const BufferInfo& _info, Allocation&& _allocation) : Buffer(_info), D3D12DeviceChild(_device), allocation(std::move(_allocation)) {
    }

    D3D12Buffer::~D3D12Buffer() {
    }
    void D3D12Buffer::Destroy() {
        MoerDelete(this);
    }
    void D3D12Buffer::SetName(const std::string_view _name) {
        debug_name.emplace(_name);
        allocation.resource->SetName(StringWiden(_name).c_str());
    }

    DescriptorIndex D3D12Buffer::CreateSrv(const BufferView& _range, ED3D12ShaderVariableType _type) {
        ASSERT(IsShaderVarCommonBuffer(_type) && IsShaderVarSrv(_type));
        if (_type == ED3D12ShaderVariableType::TypedBuffer) {
            ASSERT2(_range.format != EPixelFormat::PF_UNDEFINED, "Typed buffer must has a format");
        }
        ViewDesc viewDesc{};
        viewDesc.type        = _type;
        viewDesc.byte_offset = _range.byte_offset;
        viewDesc.format      = D3D12EnumTranslation::METoDxFormat(_range.format);
        for (const auto& [desc, index] : srv_uav_views) {
            if (desc == viewDesc) {
                return {index};
            }
        }

        const uint byte_stride = _type == ED3D12ShaderVariableType::ByteAddressBuffer ? 4 : (_type == ED3D12ShaderVariableType::StructuredBuffer ? GetStride() : D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetBitsPerUnit(viewDesc.format) / 8);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = _type == ED3D12ShaderVariableType::ByteAddressBuffer ? DXGI_FORMAT_R32_TYPELESS : (_type == ED3D12ShaderVariableType::StructuredBuffer ? DXGI_FORMAT_UNKNOWN : viewDesc.format);
        ASSERT(_range.GetByteOffset() % byte_stride == 0);
        srvDesc.Buffer.FirstElement        = _range.GetByteOffset() / byte_stride;
        srvDesc.Buffer.NumElements         = _range.GetNumElements();
        srvDesc.Buffer.StructureByteStride = _type == ED3D12ShaderVariableType::StructuredBuffer ? byte_stride : 0;
        srvDesc.Buffer.Flags               = _type == ED3D12ShaderVariableType::ByteAddressBuffer ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;

        auto index = device->GetCsuHeap()->Allocate();
        device->Native()->CreateShaderResourceView(allocation.resource, &srvDesc, device->GetCsuHeap()->GetOffsetHandleCpu(index));
        srv_uav_views.emplace_back(viewDesc, index);
        return index;
    }

    DescriptorIndex D3D12Buffer::CreateUav(const BufferView& _range, ED3D12ShaderVariableType _type) {
        ASSERT(IsShaderVarCommonBuffer(_type) && IsShaderVarUav(_type));
        ASSERT(EnumHasAnyFlag(GetUsage(), EBufferUsageFlags::UNORDERED_ACCESS));
        if (_type == ED3D12ShaderVariableType::RWTypedBuffer) {
            ASSERT2(_range.format != EPixelFormat::PF_UNDEFINED, "Typed buffer must has a format");
        }
        ViewDesc viewDesc{};
        viewDesc.type        = _type;
        viewDesc.byte_offset = _range.byte_offset;
        viewDesc.format      = D3D12EnumTranslation::METoDxFormat(_range.format);
        for (const auto& [desc, index] : srv_uav_views) {
            if (desc == viewDesc) {
                return {index};
            }
        }

        const uint byte_stride = _type == ED3D12ShaderVariableType::RWByteAddressBuffer ? 4 : (_type == ED3D12ShaderVariableType::RWStructuredBuffer ? GetStride() : D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetBitsPerUnit(viewDesc.format) / 8);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format        = _type == ED3D12ShaderVariableType::RWByteAddressBuffer ? DXGI_FORMAT_R32_TYPELESS : (_type == ED3D12ShaderVariableType::RWStructuredBuffer ? DXGI_FORMAT_UNKNOWN : viewDesc.format);
        ASSERT(_range.GetByteOffset() % byte_stride == 0);
        uavDesc.Buffer.FirstElement         = _range.GetByteOffset() / byte_stride;
        uavDesc.Buffer.NumElements          = _range.GetNumElements();
        uavDesc.Buffer.StructureByteStride  = _type == ED3D12ShaderVariableType::RWStructuredBuffer ? byte_stride : 0;
        uavDesc.Buffer.Flags                = _type == ED3D12ShaderVariableType::RWByteAddressBuffer ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
        uavDesc.Buffer.CounterOffsetInBytes = 0;

        auto index = device->GetCsuHeap()->Allocate();
        device->Native()->CreateUnorderedAccessView(allocation.resource, nullptr, &uavDesc, device->GetCsuHeap()->GetOffsetHandleCpu(index));
        srv_uav_views.emplace_back(viewDesc, index);
        return index;
    }

    D3D12GpuGlobalAllocator::D3D12GpuGlobalAllocator(D3D12Device* _device) : device(_device) {
        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice  = _device->Native();
        allocatorDesc.pAdapter = _device->NativeAdapter();
        // These flags are optional but recommended.
        allocatorDesc.Flags = static_cast<D3D12MA::ALLOCATOR_FLAGS>(// cast make it happy
            D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED | D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED);
        //| D3D12MA::ALLOCATOR_FLAG_SINGLETHREADED);
        DX_CHECK_HRESULT(D3D12MA::CreateAllocator(&allocatorDesc, d3d12Allocator.ReleaseAndGetAddressOf()));
    }

    static bool IsPowerOfTwo(uint64 _value) {
        return _value != 0 && (_value & (_value - 1)) == 0;
    }

    Allocation D3D12GpuGlobalAllocator::AllocateBufferHeap(std::string_view _name, uint64 _byte_size, D3D12_HEAP_TYPE _heap_type) {
        D3D12MA::ALLOCATION_DESC desc;
        desc.HeapType       = _heap_type;
        desc.Flags          = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT;
        desc.ExtraHeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        desc.CustomPool     = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO info;
        info.Alignment   = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        info.SizeInBytes = AlignUpToPowerOfTwo(_byte_size, info.Alignment);

        Allocation alloc;
        DX_CHECK_HRESULT(d3d12Allocator->AllocateMemory(&desc, &info, &alloc.alloc));
        alloc.alloc->SetPrivateData(device);// sometime useful
        alloc.alloc->SetName(StringWiden(_name).c_str());
        return alloc;
    }

    Allocation D3D12GpuGlobalAllocator::AllocateTextureHeap(std::string_view _name, uint64 _byte_size, uint64 _alignment, bool _is_rtv_dsv) {
        D3D12MA::ALLOCATION_DESC desc;
        desc.HeapType       = D3D12_HEAP_TYPE_DEFAULT;
        desc.Flags          = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT;
        desc.ExtraHeapFlags = _is_rtv_dsv ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        desc.CustomPool     = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO info;
        DASSERT(IsPowerOfTwo(_alignment));
        info.Alignment   = _alignment;
        info.SizeInBytes = AlignUpToPowerOfTwo(_byte_size, _alignment);

        Allocation alloc;
        DX_CHECK_HRESULT(d3d12Allocator->AllocateMemory(&desc, &info, &alloc.alloc));
        alloc.alloc->SetPrivateData(device);// sometime useful
        alloc.alloc->SetName(StringWiden(_name).c_str());
        return alloc;
    }

    void D3D12GraphicsCommandQueue::ExecuteThread(std::stop_token _st) {
        do {
            {
                std::unique_lock lck(mtx);
                if (!cv.wait(lck, _st, [&]() { return !event_queue.empty(); })) {
                    continue;
                }
            }

            uint64 fence_value;// local state

            is_second_thread_busy = true;
            while (true) {
                std::optional<QueueEvent> evt;
                {
                    std::unique_lock lck(mtx);
                    if (!event_queue.empty()) {
                        evt.emplace(std::move(event_queue.front()));
                        event_queue.pop_front();
                    }
                }
                if (!evt) break;
                fence_value = evt->timeline;
                // visit event
                evt->event.Match(
                    [&fence_value, this](UniquePtr<D3D12CommandResourceAllocator>& allocator) {
                        queue_fence.WaitOnHost(fence_value);
                        allocator->OnComplete();// callbacks,
                        allocator->Reset();
                        ready_allocators.Push(allocator.release());
                        GetDevice()->GetCsuHeapGpuDyn()->EndPushDescriptors();
                        GetDevice()->GetSamplerHeapGpuDyn()->EndPushDescriptors();
                        //LOG_INFO("visit event: allocator, {}", fence_value);
                        AtomicMaximum(last_completed_fence_value, fence_value);// break the 'Sync' condition, later
                    },
                    [&fence_value](const Array<std::function<void()>>& funcs) {
                        //LOG_INFO("visit event: callback, {}", fence_value);
                        for (auto&& f : funcs) f();
                    },
                    [&fence_value](SignalEvent e) {
                        //LOG_INFO("visit event: signal, {}", fence_value);
                        auto fence = reinterpret_cast<D3D12Fence*>(e.timeline_handle);
                        fence->SignalOnHost(e.value);
                    },
                    [&fence_value, this](WaitEvent e) {
                        LOG_INFO("visit event: wait, {}", fence_value);
                        FATAL("not implemented");
                        //auto fence = reinterpret_cast<D3D12Fence*>(e.timeline_handle);
                        //fence->WaitOnHost(e.value);
                    });
            }
            is_second_thread_busy = false;

        } while (!_st.stop_requested());
    }

    struct D3D12CommandPreprocessVisitor {
        D3D12CommandResourceAllocator& allocator;
        D3D12ResourceStateTracker&     tracker;

        D3D12CommandPreprocessVisitor(D3D12CommandResourceAllocator& allocator) : allocator(allocator), tracker(allocator.GetResourceStateTracker()) {}

        void Visit(const UploadBufferCmd& _cmd) {
            D3D12Buffer* dst_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            tracker.RecordState(dst_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
        }

        void Visit(const CopyBackBufferCmd& _cmd) {
            D3D12Buffer* src_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            tracker.RecordState(src_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_SOURCE});
        }

        void Visit(const CopyBufferCmd& _cmd) {
            D3D12Buffer* dst_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.DstHandle());
            D3D12Buffer* src_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.SrcHandle());
            tracker.RecordState(dst_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
            tracker.RecordState(src_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_SOURCE});
        }

        void Visit(const CopyBufferToTextureCmd& _cmd) {
            D3D12Texture* dst_texture = reinterpret_cast<D3D12Texture*>(_cmd.DstHandle());
            D3D12Buffer*  src_buffer  = reinterpret_cast<D3D12Buffer*>(_cmd.SrcHandle());
            tracker.RecordState(dst_texture, {.layout = D3D12_BARRIER_LAYOUT_COPY_DEST, .sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
            tracker.RecordState(src_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_SOURCE});
        }

        void Visit(const CopyTextureToBufferCmd& _cmd) {
            D3D12Buffer*  dst_buffer  = reinterpret_cast<D3D12Buffer*>(_cmd.DstHandle());
            D3D12Texture* src_texture = reinterpret_cast<D3D12Texture*>(_cmd.SrcHandle());
            tracker.RecordState(dst_buffer, {.sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
            tracker.RecordState(src_texture, {.layout = D3D12_BARRIER_LAYOUT_COPY_SOURCE, .sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_SOURCE});
        }

        void Visit(const UploadTextureCmd& _cmd) {
            D3D12Texture* dst_texture = reinterpret_cast<D3D12Texture*>(_cmd.Handle());
            tracker.RecordState(dst_texture, {.layout = D3D12_BARRIER_LAYOUT_COPY_DEST, .sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
        }

        void Visit(const CopyTextureCmd& _cmd) {
            D3D12Texture* dst_texture = reinterpret_cast<D3D12Texture*>(_cmd.DstHandle());
            D3D12Texture* src_texture = reinterpret_cast<D3D12Texture*>(_cmd.SrcHandle());
            tracker.RecordState(dst_texture, {.layout = D3D12_BARRIER_LAYOUT_COPY_DEST, .sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_DEST});
            tracker.RecordState(src_texture, {.layout = D3D12_BARRIER_LAYOUT_COPY_SOURCE, .sync = D3D12_BARRIER_SYNC_COPY, .access = D3D12_BARRIER_ACCESS_COPY_SOURCE});
        }

        void Visit(const BarrierCmd& _cmd) {
            // we not ignore _cmd.IsQueueTransition()
            ASSERT(!_cmd.IsQueueTransition());

            for (const auto& barrier : _cmd.ReadBuffers()) {
                D3D12Buffer* buffer = reinterpret_cast<D3D12Buffer*>(barrier.handle);
                tracker.RecordState(buffer, barrier.state, barrier.pass_type, false);
            }
            for (const auto& barrier : _cmd.WriteBuffers()) {
                D3D12Buffer* buffer = reinterpret_cast<D3D12Buffer*>(barrier.handle);
                tracker.RecordState(buffer, barrier.state, barrier.pass_type, true);
            }
            for (const auto& barrier : _cmd.ReadTextures()) {
                D3D12Texture* texture = reinterpret_cast<D3D12Texture*>(barrier.handle);
                tracker.RecordState(texture, barrier.state, barrier.pass_type, false);
            }
            for (const auto& barrier : _cmd.WriteTextures()) {
                D3D12Texture* texture = reinterpret_cast<D3D12Texture*>(barrier.handle);
                tracker.RecordState(texture, barrier.state, barrier.pass_type, true);
            }
        }

        void VisitArgs(const TArg& _arg, uint64 _state_flag, uint64 _pipeline_flag) {
            auto sync_flag = D3D12_BARRIER_SYNC(_pipeline_flag);
            auto var_type  = ED3D12ShaderVariableType(_state_flag);

            Variant(_arg)
                .Match(
                    [&](const TInvalidArg&) {},
                    [&](const BufferView& arg) {
                        D3D12Buffer* buf     = reinterpret_cast<D3D12Buffer*>(arg.GetBuffer());
                        const bool   b_write = IsShaderVarUav(var_type);
                        tracker.RecordState(buf,
                                            b_write ? EBufferState::UNORDERED_ACCESS : EBufferState::SHADER_RESOURCE,
                                            sync_flag == D3D12_BARRIER_SYNC_COMPUTE_SHADING ? EPassType::Compute : EPassType::Graphics,// a bit strange...
                                            b_write);
                    },
                    [&](const TextureView& arg) {
                        D3D12Texture* tex     = reinterpret_cast<D3D12Texture*>(arg.GetTexture());
                        const bool    b_write = IsShaderVarUav(var_type);
                        tracker.RecordState(tex,
                                            b_write ? ETextureState::UNORDERED_ACCESS : ETextureState::SHADER_RESOURCE,
                                            sync_flag == D3D12_BARRIER_SYNC_COMPUTE_SHADING ? EPassType::Compute : EPassType::Graphics,
                                            b_write);
                    },
                    [&](const std::span<TextureView>& arg) {
                        for (auto&& i : arg) {
                            VisitArgs(i, _state_flag, _pipeline_flag);
                        }
                    },
                    [&](const std::span<BufferView>& arg) {
                        for (auto&& i : arg) {
                            VisitArgs(i, _state_flag, _pipeline_flag);
                        }
                    },
                    [&](const Sampler& arg) {},
                    [&](const BindlessArrayRef& arg) {
                        // todo
                        FATAL("not implemented");
                    },
                    [&](const RaytracingTlasRef& arg) {
                        // todo
                        FATAL("not implemented");
                    });
        }

        void Visit(const DispatchCmd& _cmd) {
            ASSERT(std::holds_alternative<uint3>(_cmd.Param()));// todo indirect buffer
            const auto& pipeline = _cmd.Pipeline();

            auto func = [&](const TArg& _arg, uint _idx) {
                VisitArgs(_arg, pipeline.binding_infos[_idx].state_flags, pipeline.binding_infos[_idx].pipeline_flags);
            };
            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                // todo
                FATAL("not implemented");
            };

            IterateArgs(_cmd.Args({}), func, bdls_post_func);
        }

        void VisitCmd(const Command* _cmd) {
            switch (_cmd->Type()) {
                case Command::EType::UploadBuffer:
                    Visit(static_cast<const UploadBufferCmd&>(*_cmd));
                    break;
                case Command::EType::CopyBackBuffer:
                    Visit(static_cast<const CopyBackBufferCmd&>(*_cmd));
                    break;
                case Command::EType::BufferToBuffer:
                    Visit(static_cast<const CopyBufferCmd&>(*_cmd));
                    break;
                case Command::EType::BufferToTexture:
                    Visit(static_cast<const CopyBufferToTextureCmd&>(*_cmd));
                    break;
                case Command::EType::TextureToBuffer:
                    Visit(static_cast<const CopyTextureToBufferCmd&>(*_cmd));
                    break;
                case Command::EType::UploadTexture:
                    Visit(static_cast<const UploadTextureCmd&>(*_cmd));
                    break;
                case Command::EType::TextureToTexture:
                    Visit(static_cast<const CopyTextureCmd&>(*_cmd));
                    break;
                case Command::EType::Barrier:
                    Visit(static_cast<const BarrierCmd&>(*_cmd));
                    break;
                case Command::EType::ShaderDispatch:
                    Visit(static_cast<const DispatchCmd&>(*_cmd));
                    break;

                default:
                    FATAL("not implemented cmdtype {}", Command::typenames[uint(_cmd->Type())]);
            }
        }
    };

    struct D3D12CommandVisitor {
        D3D12CommandResourceAllocator& allocator;
        D3D12ResourceStateTracker&     tracker;
        D3D12CommandList&              cmd_list;

        D3D12CommandVisitor(D3D12CommandResourceAllocator& allocator) : allocator(allocator), tracker(allocator.GetResourceStateTracker()), cmd_list(*allocator.GetCommandList()) {}

        void Visit(const UploadBufferCmd& _cmd) {
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateUploadBuffer(_cmd.ByteSize(), 256);// ? not sure alignment
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
            D3D12Buffer* buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            cmd_list.CopyBuffer(tmp_buffer.buffer, buffer, _cmd.ByteSize(), tmp_buffer.byte_offset, _cmd.Offset());
        }

        void Visit(const CopyBackBufferCmd& _cmd) {
            auto         tmp_buffer = allocator.AllocateReadbackBuffer(_cmd.ByteSize(), 256);// ? not sure alignment
            D3D12Buffer* src_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            cmd_list.CopyBuffer(src_buffer, tmp_buffer.buffer, _cmd.ByteSize(), _cmd.Offset(), tmp_buffer.byte_offset);
            allocator.AddOnComplete([tmp_buffer, &cmd_list(cmd_list), dst(_cmd.Data()), size(_cmd.ByteSize())] {
                cmd_list.CopyData(dst, tmp_buffer, size);
            });
        }

        void Visit(const CopyBufferCmd& _cmd) {
            D3D12Buffer* dst_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.DstHandle());
            D3D12Buffer* src_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.SrcHandle());
            cmd_list.CopyBuffer(src_buffer, dst_buffer, _cmd.ByteSize(), _cmd.SrcOffset(), _cmd.DstOffset());
        }

        // need more test to verify
        void Visit(const CopyBufferToTextureCmd& _cmd) {
            D3D12Texture* dst_texture = reinterpret_cast<D3D12Texture*>(_cmd.DstHandle());
            D3D12Buffer*  src_buffer  = reinterpret_cast<D3D12Buffer*>(_cmd.SrcHandle());

            cmd_list.CopyBufferToTexture(src_buffer,
                                         dst_texture,
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyTextureToBufferCmd& _cmd) {
            D3D12Texture* src_texture = reinterpret_cast<D3D12Texture*>(_cmd.SrcHandle());
            D3D12Buffer*  dst_buffer  = reinterpret_cast<D3D12Buffer*>(_cmd.DstHandle());

            cmd_list.CopyTextureToBuffer(src_texture,
                                         dst_buffer,
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const UploadTextureCmd& _cmd) {
            // consider padding TODO
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateUploadBuffer(data_span.size_bytes(), 256);// ? not sure alignment
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());

            D3D12Texture* texture = reinterpret_cast<D3D12Texture*>(_cmd.Handle());
            cmd_list.CopyBufferToTexture(tmp_buffer.buffer,
                                         texture,
                                         tmp_buffer.byte_offset,
                                         _cmd.Offset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyTextureCmd& _cmd) {
            D3D12Texture* src_texture = reinterpret_cast<D3D12Texture*>(_cmd.SrcHandle());
            D3D12Texture* dst_texture = reinterpret_cast<D3D12Texture*>(_cmd.DstHandle());

            cmd_list.CopyTexture(src_texture,
                                 dst_texture,
                                 _cmd.Size(),
                                 _cmd.SrcOffset(),
                                 _cmd.DstOffset(),
                                 _cmd.SrcMipLevel(),
                                 _cmd.DstMipLevel());
        }

        void Visit(const DispatchCmd& _cmd) {
            PipelineHandle& pso = _cmd.Pipeline();
            cmd_list.SetPso(pso);

            const auto& args = _cmd.Args({});// todo cached args
            ASSERT(args.Size() > 0);

            cmd_list.BindDescriptors(pso, args);

            std::visit(
                Overload{
                    [&](uint3 _param) {
                        cmd_list.Dispatch(_param.x, _param.y, _param.z);
                    },
                    [&](const DispatchIndirectParam& _param) {
                        //todo execute indirect. command signature
                        ASSERT(false);
                    }},
                _cmd.Param());
        }

        void VisitCmd(const Command* _cmd) {
            switch (_cmd->Type()) {
                case Command::EType::UploadBuffer:
                    Visit(static_cast<const UploadBufferCmd&>(*_cmd));
                    break;
                case Command::EType::CopyBackBuffer:
                    Visit(static_cast<const CopyBackBufferCmd&>(*_cmd));
                    break;
                case Command::EType::BufferToBuffer:
                    Visit(static_cast<const CopyBufferCmd&>(*_cmd));
                    break;
                case Command::EType::BufferToTexture:
                    Visit(static_cast<const CopyBufferToTextureCmd&>(*_cmd));
                    break;
                case Command::EType::TextureToBuffer:
                    Visit(static_cast<const CopyTextureToBufferCmd&>(*_cmd));
                    break;
                case Command::EType::UploadTexture:
                    Visit(static_cast<const UploadTextureCmd&>(*_cmd));
                    break;
                case Command::EType::TextureToTexture:
                    Visit(static_cast<const CopyTextureCmd&>(*_cmd));
                    break;
                case Command::EType::Barrier:
                    break;// no-op
                case Command::EType::ShaderDispatch:
                    Visit(static_cast<const DispatchCmd&>(*_cmd));
                    break;
                    //CopyBackTexture,
                    //BuildAccel,
                    //BuildTLAS,
                    //TraceRay,
                    //Barrier,
                    //QueueTransfer,
                    //SetDrawState,
                    //SetGeometryPassDrawState,
                    //UpdateBindlessArray,
                    //ClearResource,
                    //Scope,
                default:
                    FATAL("not implemented cmdtype {}", Command::typenames[uint(_cmd->Type())]);
            }
        }
    };

    UniquePtr<D3D12CommandResourceAllocator> D3D12GraphicsCommandQueue::RequestCommandResourceAllocator() {
        if (pending_fence_values.Full()) {
            Complete(pending_fence_values.Front());
        }
        auto allocator = UniquePtr<D3D12CommandResourceAllocator>(ready_allocators.Pop());
        if (allocator) {
            return std::move(allocator);
        }
        //static uint32_t count = 0;
        //count++;
        //LOG_WARNING("create allocator {}", count);
        return MakeUnique<D3D12CommandResourceAllocator>(*this);
    }

    void D3D12GraphicsCommandQueue::Complete(uint64 fence_value) {
        while (last_completed_fence_value < fence_value) {
            std::this_thread::yield();
        }
    }

    D3D12GraphicsCommandQueue::D3D12GraphicsCommandQueue(D3D12Device* _device, EQueueType _type)
        : CommandQueue(), D3D12DeviceChild(_device),
          queue_type(D3D12_COMMAND_LIST_TYPE_DIRECT),
          last_submitted_fence_value(0),
          last_completed_fence_value(0),
          thd([this](std::stop_token _st) { ExecuteThread(_st); }),
          queue_fence(_device) {
        ASSERT(_type == EQueueType::Graphics);// ?

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
        DX_CHECK_HRESULT(device->Native()->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf())));
        queue->SetName(StringWiden("GraphicsCommandQueue").c_str());
    }

    D3D12GraphicsCommandQueue::~D3D12GraphicsCommandQueue() {
        //is_thread_enable = false;
        cv.notify_all();
        thd.join();
    }

    void D3D12GraphicsCommandQueue::Wait(WaitEvent _event) {
        FATAL("not implemented");
    }

    WaitEvent D3D12GraphicsCommandQueue::Execute(CmdSubmit&& _submit) {

        auto allocator = RequestCommandResourceAllocator();
        auto cmd_list  = allocator->GetCommandList();

        //FunctionTable function_table{
        //    .is_resource_write       = &IsBufferTextureWrite,
        //    .is_resource_read        = &IsBufferTextureRead,
        //    .is_texture_sampled      = &IsTextureSampled,
        //    .is_resource_in_bindless = &IsResourceInBindlessArray};
        //CmdReorderer reorderer{function_table};
        //for (const auto& cmd : _submit.cmds) {
        //    reorderer.AcceptCmd(cmd.get());
        //}
        //const auto& cmd_lists = reorderer.m_cmd_lists;

        cmd_list->Begin();
        if (!_submit.cmds.empty()) {
            D3D12CommandVisitor           cmd_visitor(*allocator);
            D3D12CommandPreprocessVisitor preprocess_visitor(*allocator);
            auto&                         tracker = allocator->GetResourceStateTracker();

            // prepare descriptor heaps
            ID3D12DescriptorHeap* heaps[] = {device->GetCsuHeapGpuDyn()->Native(), device->GetSamplerHeapGpuDyn()->Native()};
            cmd_list->Native()->SetDescriptorHeaps(2, heaps);
            device->GetCsuHeapGpuDyn()->BeginPushDescriptors();
            device->GetSamplerHeapGpuDyn()->BeginPushDescriptors();

            for (const auto& cmd : _submit.cmds) {// todo reorder
                preprocess_visitor.VisitCmd(cmd.get());

                tracker.ResolveBarriers();
                tracker.DispatchBarriers(cmd_list->Native());

                cmd_visitor.VisitCmd(cmd.get());
            }
            {
                tracker.RestoreState();
                tracker.DispatchBarriers(cmd_list->Native());
                tracker.Reset();
            }
        }
        cmd_list->End();

        const uint64 next_fence_value = ++last_submitted_fence_value;

        allocator->AddOnComplete([next_fence_value, this] {
            LOG_INFO("on complete {}", next_fence_value);
        });

        for (auto&& e : _submit.wait_events) {
            queue->Wait(reinterpret_cast<D3D12Fence*>(e.timeline_handle)->Native(), e.value);
        }
        {
            ID3D12CommandList* ppCommandLists[]{cmd_list->Native()};
            queue->ExecuteCommandLists(1, ppCommandLists);
        }
        queue->Signal(queue_fence.Native(), next_fence_value);
        for (auto&& e : _submit.signal_events) {
            queue->Signal(reinterpret_cast<D3D12Fence*>(e.timeline_handle)->Native(), e.value);
        }

        // TODO our event queue
        {
            std::unique_lock lck(mtx);

            // to reset allocator
            event_queue.emplace_back(std::move(allocator), next_fence_value, true);

            if (!_submit.callbacks.empty() || !_submit.signal_events.empty()) {

                // signal fence after execute
                for (auto&& e : _submit.signal_events) {
                    event_queue.emplace_back(std::move(e), next_fence_value, false);
                }

                // call backs after execute
                if (!_submit.callbacks.empty()) {
                    event_queue.emplace_back(std::move(_submit.callbacks), next_fence_value, true);
                }
            }
            cv.notify_one();
        }
        pending_fence_values.Enqueue(next_fence_value);

        return WaitEvent(uint64(&queue_fence), next_fence_value);
    }

    void D3D12GraphicsCommandQueue::Present(SwapchainRef _swapchain, TextureView _target) {
        FATAL("not implemented");
    }

    void D3D12GraphicsCommandQueue::Sync() {
        const uint64 target = last_submitted_fence_value;
        if (device->config.force_sync) {
            while (last_completed_fence_value < target || is_second_thread_busy) {
                std::this_thread::yield();
            }
        } else {
            Complete(target);
        }
        //queue_fence.WaitOnHost(last_submitted_fence_value);  // here can't just use gpu to signal fence, need to wait more for cpu work
    }

    D3D12Fence::D3D12Fence(D3D12Device* _device) : D3D12DeviceChild(_device) {
        DX_CHECK_HRESULT(device->Native()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())));
        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            DX_CHECK_HRESULT(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    D3D12Fence::~D3D12Fence() {
        CloseHandle(event);
    }

    uint64_t D3D12Fence::GetValue() const {
        //return current_value;
        return fence->GetCompletedValue();
    }

    void D3D12Fence::Wait(uint64_t _value) {
        WaitOnHost(_value);
    }

    bool D3D12Fence::IsFenceComplete(uint64 _value) {
        // ref directx-graphics-samples
        // maybe need a lock..
        if (current_value < _value) {
            AtomicMaximum(current_value, fence->GetCompletedValue());
        }
        return current_value >= _value;
    }

    void D3D12Fence::WaitOnHost(uint64 _value) {
        if (IsFenceComplete(_value))
            return;
        fence->SetEventOnCompletion(_value, event);
        WaitForSingleObjectEx(event, INFINITE, FALSE);
        //OnScopeExit([&]() { ResetEvent(event); });// maybe useful
        //ResetEvent(event);
    }

    void D3D12Fence::SignalOnHost(uint64 _value) {
        DX_CHECK_HRESULT(fence->Signal(_value));// ?
    }

    D3D12CommandResourceAllocator::D3D12CommandResourceAllocator(D3D12GraphicsCommandQueue& _queue)
        : D3D12DeviceChild(_queue.GetDevice()),
          queue(_queue),
          upload_allocator(device, D3D12_HEAP_TYPE_UPLOAD, 4 * 1024 * 1024, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),// ref FD3D12UploadHeapAllocator
          readback_allocator(device, D3D12_HEAP_TYPE_READBACK, 4 * 1024 * 1024, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
          constant_allocator(device, 4 * 1024 * 1024, 64 * 1024),
          default_buffer_allocator{*this},
          scratch_buffer_allocator{*this} {
        DX_CHECK_HRESULT(device->Native()->CreateCommandAllocator(queue.GetType(), IID_PPV_ARGS(cmd_allocator.ReleaseAndGetAddressOf())));
        cmd_allocator->SetName(L"GraphicsCommandAllocator");
        cmd_list = MakeUnique<D3D12CommandList>(*this);
    }

    void D3D12CommandResourceAllocator::Reset() {
        cmd_allocator->Reset();
        on_complete_callbacks.clear();
        default_buffer_allocator.Reset();
        scratch_buffer_allocator.Reset();
        upload_allocator.CleanUpAllocations();
        readback_allocator.CleanUpAllocations();
        for (auto&& buffer : large_buffers) {
            buffer->Native()->Unmap(0, nullptr);
        }
        large_buffers.clear();
    }

    static UniquePtr<D3D12Buffer> CreateLargeStagingBuffer(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint64 _byte_size) {
        const auto           heap_prop    = CD3DX12_HEAP_PROPERTIES(_heap_type);
        D3D12_RESOURCE_DESC1 resourceDesc = CD3DX12_RESOURCE_DESC1::Buffer(_byte_size);
        if (_heap_type == D3D12_HEAP_TYPE_READBACK) {
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }

        Allocation allocation;
        _device->Native()->CreateCommittedResource3(
            &heap_prop,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_BARRIER_LAYOUT_UNDEFINED,
            nullptr,
            nullptr,
            0,
            nullptr,
            IID_PPV_ARGS(&allocation.resource));
        return MakeUnique<D3D12Buffer>(_device, BufferInfo{_byte_size, 1, EBufferUsageFlags::CPU_VISIBLE}, std::move(allocation));
    }

    D3D12StagingBufferView D3D12CommandResourceAllocator::AllocateConstantBuffer(uint32 _size) {
        return constant_allocator.Allocate(_size);
    }

    D3D12StagingBufferView D3D12CommandResourceAllocator::AllocateUploadBuffer(uint64 _size, uint32 _alignment) {
        if (_size < larget_buffer_byte_size_threshold) {
            return upload_allocator.Allocate(_size, _alignment);
        }
        // allocate large upload buffer
        large_buffers.emplace_back(CreateLargeStagingBuffer(device, D3D12_HEAP_TYPE_UPLOAD, _size));
        auto*       buffer = large_buffers.back().get();
        uint8*      mapped;
        D3D12_RANGE read_range{0, 0};
        DX_CHECK_HRESULT(buffer->Native()->Map(0, &read_range, reinterpret_cast<void**>(&mapped)));
        return D3D12StagingBufferView{buffer, 0, buffer->GetGpuVirtualAddress(), mapped};
    }

    D3D12StagingBufferView D3D12CommandResourceAllocator::AllocateReadbackBuffer(uint64 _size, uint32 _alignment) {
        if (_size < larget_buffer_byte_size_threshold) {
            return readback_allocator.Allocate(_size, _alignment);
        }
        // allocate large readback buffer
        large_buffers.emplace_back(CreateLargeStagingBuffer(device, D3D12_HEAP_TYPE_READBACK, _size));
        auto*  buffer = large_buffers.back().get();
        uint8* mapped;
        DX_CHECK_HRESULT(buffer->Native()->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        return D3D12StagingBufferView{buffer, 0, buffer->GetGpuVirtualAddress(), mapped};
    }

    D3D12Buffer* D3D12CommandResourceAllocator::AllocateScratch(uint64 _size) {
        return scratch_buffer_allocator.Allocate(_size, EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH);
    }

    D3D12Buffer* D3D12CommandResourceAllocator::AllocateDefaultBuffer(uint64 _size) {
        return default_buffer_allocator.Allocate(_size, EBufferUsageFlags::NONE);
    }

    D3D12CommandList::D3D12CommandList(D3D12CommandResourceAllocator& allocator) : allocator(allocator) {
        DX_CHECK_HRESULT(allocator.device->Native()->CreateCommandList1(0, allocator.queue.GetType(), D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(list.ReleaseAndGetAddressOf())));
        list->SetName(L"GraphicsCommandList");
    }

    void D3D12CommandList::Begin() {
        DX_CHECK_HRESULT(list->Reset(allocator.Native(), nullptr));
    }

    void D3D12CommandList::End() {
        DX_CHECK_HRESULT(list->Close());
    }
    void D3D12CommandList::SetPso(const PipelineHandle& _handle) {
        auto* pso = reinterpret_cast<D3D12PipelineState*>(_handle.handle);
        list->SetPipelineState(pso->Native());
        list->SetComputeRootSignature(pso->NativeRootSignature());
    }
    void D3D12CommandList::BindDescriptors(PipelineHandle& _pso_handle, const ArrayArguments& _args) {

        D3D12PipelineState* pso    = reinterpret_cast<D3D12PipelineState*>(_pso_handle.handle);
        const auto&         layout = pso->GetLayout();

        if (layout.root_constant) {
            ASSERT(layout.root_constant->idx_in_cpp_args < _args.Size() && layout.root_constant->idx_in_cpp_args < _pso_handle.binding_infos.size());
            const ParamInfoFlags flags    = _pso_handle.binding_infos[layout.root_constant->idx_in_cpp_args];
            const auto           var_type = ED3D12ShaderVariableType(flags.state_flags);
            ASSERT(IsShaderVarRootConstant(var_type));
            const auto& constants = _args.constants;
            list->SetComputeRoot32BitConstants(layout.root_constant->idx_in_root_sig, constants.size(), constants.data(), 0);
        }
        if (layout.root_cbvs) {
            for (const auto& cbv : *layout.root_cbvs) {
                ASSERT(cbv.idx_in_cpp_args < _args.Size() && cbv.idx_in_cpp_args < _pso_handle.binding_infos.size());
                const ParamInfoFlags flags    = _pso_handle.binding_infos[cbv.idx_in_cpp_args];
                const auto           var_type = ED3D12ShaderVariableType(flags.state_flags);
                ASSERT(IsShaderVarConstantBuffer(var_type));
                ASSERT(std::holds_alternative<BufferView>(_args[cbv.idx_in_cpp_args]));
                const auto&  buf_view = std::get<BufferView>(_args[cbv.idx_in_cpp_args]);
                D3D12Buffer* buf      = reinterpret_cast<D3D12Buffer*>(buf_view.GetBuffer());
                ASSERT(buf_view.GetByteOffset() % 256 == 0);                                                                        // ?
                list->SetComputeRootConstantBufferView(cbv.idx_in_root_sig, buf->GetGpuVirtualAddress() + buf_view.GetByteOffset());// device-side buffer
            }
        }
        if (layout.the_bindless_array) {
            FATAL("not implemented");
            //ASSERT(layout.the_bindless_array->idx_in_cpp_args < _args.Size() && layout.the_bindless_array->idx_in_cpp_args < _pso_handle.binding_infos.size());
            //const ParamInfoFlags flags    = _pso_handle.binding_infos[layout.the_bindless_array->idx_in_cpp_args];
            //const auto           var_type = ED3D12ShaderVariableType(flags.state_flags);
            //ASSERT(IsShaderVarCommonBuffer(var_type) && IsShaderVarSrv(var_type));
            //ASSERT(std::holds_alternative<BindlessArrayRef>(_args[layout.the_bindless_array->idx_in_cpp_args]));
            //const auto&  buf_view = std::get<BindlessArrayRef>(_args[layout.the_bindless_array->idx_in_cpp_args]);
            //D3D12Buffer* buf      = reinterpret_cast<D3D12Buffer*>(buf_view.GetBuffer());
            //ASSERT(buf_view.GetByteOffset() == 0); // ?
            //list->SetComputeRootShaderResourceView(layout.the_bindless_array->idx_in_root_sig, buf->GetGpuVirtualAddress());
        }
        if (layout.srv_table) {
            Array<DescriptorIndex> indices(layout.srv_table->GetTotalBindCount());

            for (const auto& e : layout.srv_table->entries) {
                ASSERT(e.idx_in_cpp_args < _args.Size() && e.idx_in_cpp_args < _pso_handle.binding_infos.size());
                const ParamInfoFlags flags    = _pso_handle.binding_infos[e.idx_in_cpp_args];
                const auto           var_type = ED3D12ShaderVariableType(flags.state_flags);
                ASSERT(IsShaderVarSrv(var_type));

                Variant(_args[e.idx_in_cpp_args])
                    .Match(
                        // now assume all input resource valid, not consider null descriptor
                        [&](const BufferView& arg) {
                            D3D12Buffer* buf = reinterpret_cast<D3D12Buffer*>(arg.GetBuffer());
                            ASSERT(IsShaderVarCommonBuffer(var_type));
                            ASSERT(e.idx_in_table < indices.size());
                            indices[e.idx_in_table] = buf->CreateSrv(arg, var_type);
                        },
                        [&](const TextureView& arg) {
                            FATAL("not implemented");
                        },
                        [&](const std::span<TextureView>& arg) {
                            FATAL("not implemented");
                        },
                        [&](const std::span<BufferView>& arg) {
                            ASSERT(IsShaderVarCommonBuffer(var_type));
                            for (int j = 0; const BufferView& view : arg) {
                                D3D12Buffer* buf = reinterpret_cast<D3D12Buffer*>(view.GetBuffer());
                                ASSERT(e.idx_in_table + j < indices.size());
                                indices[e.idx_in_table + j] = buf->CreateSrv(view, var_type);
                                j++;
                            }
                        },
                        [&](const RaytracingTlasRef& arg) {
                            FATAL("not implemented");
                        },
                        [&](const auto&) {
                            FATAL("not support arg");
                        });
            }
            list->SetComputeRootDescriptorTable(layout.srv_table->idx_in_root_sig, allocator.GetDevice()->PushCsuDescriptor(indices));
        }
        if (layout.uav_table) {// todo unify
            Array<DescriptorIndex> indices(layout.uav_table->GetTotalBindCount());

            for (const auto& e : layout.uav_table->entries) {
                ASSERT(e.idx_in_cpp_args < _args.Size() && e.idx_in_cpp_args < _pso_handle.binding_infos.size());
                const ParamInfoFlags flags    = _pso_handle.binding_infos[e.idx_in_cpp_args];
                const auto           var_type = ED3D12ShaderVariableType(flags.state_flags);
                ASSERT(IsShaderVarUav(var_type));

                Variant(_args[e.idx_in_cpp_args])
                    .Match(
                        [&](const BufferView& arg) {
                            D3D12Buffer* buf = reinterpret_cast<D3D12Buffer*>(arg.GetBuffer());
                            ASSERT(IsShaderVarCommonBuffer(var_type));
                            ASSERT(e.idx_in_table < indices.size());
                            indices[e.idx_in_table] = buf->CreateUav(arg, var_type);
                        },
                        [&](const TextureView& arg) {
                            FATAL("not implemented");
                        },
                        [&](const std::span<TextureView>& arg) {
                            FATAL("not implemented");
                        },
                        [&](const std::span<BufferView>& arg) {
                            ASSERT(IsShaderVarCommonBuffer(var_type));
                            for (int j = 0; const BufferView& view : arg) {
                                D3D12Buffer* buf = reinterpret_cast<D3D12Buffer*>(view.GetBuffer());
                                ASSERT(e.idx_in_table + j < indices.size());
                                indices[e.idx_in_table + j] = buf->CreateUav(view, var_type);
                                j++;
                            }
                        },
                        [&](const RaytracingTlasRef& arg) {
                            FATAL("not implemented");
                        },
                        [&](const auto&) {
                            FATAL("not support arg");
                        });
            }
            list->SetComputeRootDescriptorTable(layout.uav_table->idx_in_root_sig, allocator.GetDevice()->PushCsuDescriptor(indices));
        }
        if (layout.sampler_table) {
            FATAL("not implemented");
        }
    }
    void D3D12CommandList::Dispatch(uint _x, uint _y, uint _z) {
        list->Dispatch(_x, _y, _z);
    }
    void D3D12CommandList::CopyBuffer(D3D12Buffer* _src, D3D12Buffer* _dst, uint64 _size, uint64 _src_offset, uint64 _dst_offset) {
        list->CopyBufferRegion(
            _dst->Native(),
            _dst_offset,
            _src->Native(),
            _src_offset,
            _size);
    }
    void D3D12CommandList::CopyData(D3D12StagingBufferView _dst, const void* _data, uint64 _size) {
        std::memcpy(_dst.cpu_base_address_remapped, _data, _size);
    }
    void D3D12CommandList::CopyData(void* _dst, D3D12StagingBufferView _src, uint64 _size) {
        std::memcpy(_dst, _src.cpu_base_address_remapped, _size);
    }

    // TODO in terms of texture coordinates, do we need to flip y?
    // maybe add some validation
    // note here we assume the buffer data match the texture row pitch
    // f**k silly api https://gamedev.net/forums/topic/677932-getcopyablefootprints-question/
    void D3D12CommandList::CopyBufferToTexture(D3D12Buffer* _src, D3D12Texture* _dst, uint64 _src_offset, uint3 _dst_offset, uint3 _dst_extent, uint32 _mip_level) {
        CD3DX12_TEXTURE_COPY_LOCATION dst(_dst->Native(), _dst->QuerySubresourceIndex(_mip_level, 0, 0));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
        {
            const auto desc          = _dst->Native()->GetDesc();
            uint64     required_size = 0, y;
            uint32     x;
            allocator.GetDevice()->Native()->GetCopyableFootprints(&desc, dst.SubresourceIndex, 1, _src_offset, &layout, nullptr, nullptr, &required_size);
            DASSERT(required_size < uint64(-1) && required_size + _src_offset <= _src->GetByteSize());
        }
        CD3DX12_TEXTURE_COPY_LOCATION src(_src->Native(), layout);
        list->CopyTextureRegion(
            &dst,
            _dst_offset.x,
            _dst_offset.y,
            _dst_offset.z,
            &src,
            nullptr);
    }
    void D3D12CommandList::CopyTextureToBuffer(D3D12Texture* _src, D3D12Buffer* _dst, uint3 _src_offset, uint64 _dst_offset, uint3 _src_extent, uint32 _mip_level) {
        CD3DX12_TEXTURE_COPY_LOCATION src(_src->Native(), _src->QuerySubresourceIndex(_mip_level, 0, 0));
        CD3DX12_BOX                   srcBox(
            _src_offset.x,// what if offset!=0 && miplevel!=0 ??
            _src_offset.y,
            _src_offset.z,
            _src_offset.x + std::max(1u, _src_extent.x >> _mip_level),
            _src_offset.y + std::max(1u, _src_extent.y >> _mip_level),
            _src_offset.z + std::max(1u, _src_extent.z >> _mip_level));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        {
            const auto desc          = _src->Native()->GetDesc();
            uint64     required_size = 0;
            allocator.GetDevice()->Native()->GetCopyableFootprints(&desc, src.SubresourceIndex, 1, _dst_offset, &footprint, nullptr, nullptr, &required_size);
            DASSERT(required_size < uint64(-1) && required_size + _dst_offset <= _dst->GetByteSize());
        }
        CD3DX12_TEXTURE_COPY_LOCATION dst(_dst->Native(), footprint);
        list->CopyTextureRegion(
            &dst,
            0,// already has offset in footprint ?
            0,
            0,
            &src,
            &srcBox);
    }
    void D3D12CommandList::CopyTexture(D3D12Texture* _src, D3D12Texture* _dst, uint3 _extent, uint3 _src_offset, uint3 _dst_offset, uint32 _src_mip_level, uint32 _dst_mip_level) {
        CD3DX12_TEXTURE_COPY_LOCATION dst(_dst->Native(), _dst->QuerySubresourceIndex(_dst_mip_level, 0, 0));
        CD3DX12_TEXTURE_COPY_LOCATION src(_src->Native(), _src->QuerySubresourceIndex(_src_mip_level, 0, 0));

        CD3DX12_BOX srcBox(
            _src_offset.x,
            _src_offset.y,
            _src_offset.z,
            _src_offset.x + _extent.x,
            _src_offset.y + _extent.y,
            _src_offset.z + _extent.z);

        list->CopyTextureRegion(
            &dst,
            _dst_offset.x,
            _dst_offset.y,
            _dst_offset.z,
            &src,
            &srcBox);
    }
    D3D12Buffer* D3D12CommandResourceAllocator::HeapPlacedBufferAllocator::Allocate(uint64 _size, EBufferUsageFlags _usage) {
        allocated.emplace_back(MakeUnique<D3D12Buffer>(parent.GetDevice(), BufferInfo{_size, 1, _usage}));
        return allocated.back().get();
    }

    void D3D12CommandResourceAllocator::HeapPlacedBufferAllocator::Reset() {
        allocated.clear();
    }

    D3D12BuddyAllocator::D3D12BuddyAllocator(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _total_byte_size, uint32 _min_block_byte_size)
        : D3D12DeviceChild(_device),
          total_byte_size(_total_byte_size),
          min_block_byte_size(_min_block_byte_size),
          heap_type(_heap_type),
          max_order(BlockCountToOrder(SizeToBlockCount(_total_byte_size))),
          total_used_byte_size(0),
          ptr_mapped(nullptr) {

        ASSERT(_heap_type == D3D12_HEAP_TYPE_UPLOAD || _heap_type == D3D12_HEAP_TYPE_READBACK);
        ASSERT(_min_block_byte_size <= _total_byte_size);
        ASSERT(_total_byte_size % _min_block_byte_size == 0);
        ASSERT(IsPowerOfTwo(_min_block_byte_size));
    }

    D3D12BuddyAllocator::~D3D12BuddyAllocator() {
        if (ptr_mapped) {
            const D3D12_RANGE write_range{0, 0};
            underlying_buffer->Native()->Unmap(0, &write_range);
        }
    }

    D3D12BuddyAllocator::BuddyBlock D3D12BuddyAllocator::Allocate(uint32 _size, uint32 _alignment) {
        if (underlying_buffer == nullptr) [[unlikely]] {
            Initialize();
        }
        return AllocateInternal(_size, _alignment);
    }
    void D3D12BuddyAllocator::Deallocate(const BuddyBlock& _block) {
        if (underlying_buffer == nullptr) [[unlikely]] {
            return;
        }
        deferred_deallocate_blocks.emplace_back(_block.block_offset, _block.block_order);
    }
    void D3D12BuddyAllocator::CleanUpAllocations() {
        for (auto&& b : deferred_deallocate_blocks) {
            //LOG_INFO("[{}] deallocate block {} {}", heap_type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback", b.offset, b.order);
            DeallocateInternal(b);
        }
        deferred_deallocate_blocks.clear();
    }

    void D3D12BuddyAllocator::Initialize() {
        free_blocks.clear();
        free_blocks.resize(max_order + 1);
        free_blocks[max_order].insert(0);

        // Create the underlying buffer
        underlying_buffer = CreateLargeStagingBuffer(device, heap_type, total_byte_size);
        underlying_buffer->Native()->SetName(StringWiden(std::format("D3D12BuddyAllocator-back-buffer-{}", heap_type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback")).c_str());
        DX_CHECK_HRESULT(underlying_buffer->Native()->Map(0, nullptr, reinterpret_cast<void**>(&ptr_mapped)));
    }
    uint32 D3D12BuddyAllocator::GetSizeToAllocate(uint32 _size, uint32 _alignment) {
        if (_alignment != 0 && min_block_byte_size % _alignment != 0) {
            return _size + _alignment;
        }
        return _size;
    }
    bool D3D12BuddyAllocator::CanAllocate(uint32 _required_size) {
        if (total_used_byte_size == total_byte_size) {
            return false;
        }
        uint32 block_size = total_byte_size;
        for (int i = free_blocks.size() - 1; i >= 0; --i) {
            if (!free_blocks[i].empty() && block_size >= _required_size) {
                return true;
            }
            block_size >>= 1;
            if (_required_size > block_size) return false;
        }
        return false;
    }
    D3D12BuddyAllocator::BuddyBlock D3D12BuddyAllocator::AllocateInternal(uint32 _size, uint32 _alignment) {
        const uint32 size_to_allocate = GetSizeToAllocate(_size, _alignment);
        if (!CanAllocate(size_to_allocate)) {
            return BuddyBlock();
        }
        const uint32 block_count                = SizeToBlockCount(size_to_allocate);
        const uint32 block_order                = BlockCountToOrder(block_count);
        const uint32 offset                     = AllocateBlock(block_order);
        const uint32 allocated_size             = min_block_byte_size * block_count;
        const uint32 allocation_offset          = min_block_byte_size * offset;
        uint32       aligned_alloctation_offset = allocation_offset;
        if (_alignment != 0 && allocation_offset % _alignment != 0) {
            aligned_alloctation_offset = (aligned_alloctation_offset + _alignment - 1) / _alignment * _alignment;
            const uint32 padding       = aligned_alloctation_offset - allocation_offset;
            DASSERT(padding + _size <= allocated_size);
        }
        total_used_byte_size += allocated_size;
        DASSERT(total_used_byte_size <= total_byte_size);

        //LOG_INFO("[{}] allocate block {} {}", heap_type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback", offset, block_order);

        return BuddyBlock{
            .parent             = this,
            .block_offset       = offset,
            .block_order        = block_order,
            .memory_byte_offset = aligned_alloctation_offset,
            .required_size      = _size,
        };
    }
    uint32 D3D12BuddyAllocator::AllocateBlock(uint32 _order) {
        uint32 offset;
        ASSERT(_order <= max_order);

        if (free_blocks[_order].empty()) {
            // No free nodes in the requested pool.  Try to find a higher-order block and split it.
            const uint32 left = AllocateBlock(_order + 1);

            const uint32 size = OrderToBlockCount(_order);

            const uint32 right = left + size;

            free_blocks[_order].insert(right);// Add the right block to the free pool

            offset = left;// Return the left block
        } else {
            offset = *free_blocks[_order].begin();
            free_blocks[_order].erase(free_blocks[_order].begin());
        }

        return offset;
    }
    void D3D12BuddyAllocator::DeallocateInternal(RetiredBlock _block) {
        DeallocateBlock(_block.offset, _block.order);
        total_used_byte_size -= min_block_byte_size * OrderToBlockCount(_block.order);
    }
    void D3D12BuddyAllocator::DeallocateBlock(uint32 _offset, uint32 _order) {
        const uint32 count    = OrderToBlockCount(_order);
        const uint32 buddy_id = _offset ^ count;// ?

        if (auto it = free_blocks[_order].find(buddy_id); it != free_blocks[_order].end())// If buddy block is free, merge it
        {
            // Deallocate merged blocks
            DeallocateBlock(std::min(_offset, buddy_id), _order + 1);

            // Remove the buddy from the free list
            free_blocks[_order].erase(it);
        } else {
            // Add the block to the free list
            free_blocks[_order].insert(_offset);
        }
    }

    D3D12MultiBuddyAllocator::D3D12MultiBuddyAllocator(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _each_total_byte_size, uint32 _min_block_byte_size)
        : D3D12DeviceChild(_device),
          each_total_byte_size(_each_total_byte_size),
          min_block_byte_size(_min_block_byte_size),
          heap_type(_heap_type) {
    }
    D3D12BuddyAllocator::BuddyBlock D3D12MultiBuddyAllocator::Allocate(uint32 _size, uint32 _alignment) {
        for (auto& allocator : allocators) {
            if (auto b = allocator.Allocate(_size, _alignment); b.IsValid()) {
                return b;
            }
        }
        allocators.emplace_back(device, heap_type, each_total_byte_size, min_block_byte_size);
        return allocators.back().Allocate(_size, _alignment);
    }
    void D3D12MultiBuddyAllocator::CleanUpAllocations() {
        for (auto& allocator : allocators) {
            allocator.CleanUpAllocations();
        }
    }
    D3D12MultiBuddyAllocatorAutoFree::D3D12MultiBuddyAllocatorAutoFree(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _each_total_byte_size, uint32 _min_block_byte_size)
        : D3D12DeviceChild(_device), allocator(_device, _heap_type, _each_total_byte_size, _min_block_byte_size) {
    }
    D3D12StagingBufferView D3D12MultiBuddyAllocatorAutoFree::Allocate(uint32 _size, uint32 _alignment) {
        const auto b = allocator.Allocate(_size, _alignment);
        if (allocated_blocks.empty() || allocated_blocks.back().back().parent != b.parent) {
            allocated_blocks.emplace_back();
        }
        const auto& block = allocated_blocks.back().emplace_back(b);
        return D3D12StagingBufferView{
            .buffer                    = block.GetUnderlyingBuffer(),
            .byte_offset               = block.memory_byte_offset,
            .gpu_base_address_remapped = block.GetGpuVirtualAddressRemapped(),
            .cpu_base_address_remapped = block.GetCpuAddressRemapped(),
        };
    }
    void D3D12MultiBuddyAllocatorAutoFree::CleanUpAllocations() {
        for (auto&& blocks : allocated_blocks) {
            for (auto&& b : blocks) {
                b.parent->Deallocate(b);
            }
        }
        allocated_blocks.clear();
        allocator.CleanUpAllocations();
    }
    D3D12FastConstantAllocator::D3D12FastConstantAllocator(D3D12Device* _device, uint32 _total_byte_size, uint32 _min_block_byte_size)
        : D3D12DeviceChild(_device), allocator(_device, D3D12_HEAP_TYPE_UPLOAD, _total_byte_size, _min_block_byte_size),
          page_size(_min_block_byte_size),
          offset(page_size),// to automatic trigger the first allocation
          underlying_current{} {
    }
    D3D12StagingBufferView D3D12FastConstantAllocator::Allocate(uint32 _size) {
        DASSERT(_size < page_size);
        const uint32 aligned_size = AlignConstantBuffer(_size);
        if (offset + aligned_size > page_size) {
            underlying_current = allocator.Allocate(page_size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            offset             = 0;
            ASSERT(underlying_current.byte_offset == 0);
            ASSERT(underlying_current.gpu_base_address_remapped == underlying_current.buffer->GetGpuVirtualAddress());
        }

        auto ret = D3D12StagingBufferView{
            .buffer                    = underlying_current.buffer,
            .byte_offset               = offset,
            .gpu_base_address_remapped = underlying_current.gpu_base_address_remapped + offset,
            .cpu_base_address_remapped = underlying_current.cpu_base_address_remapped + offset,
        };

        offset += aligned_size;
        return ret;
    }
    void D3D12FastConstantAllocator::CleanUpAllocations() {
        allocator.CleanUpAllocations();
    }

    D3D12Texture::D3D12Texture(D3D12Device* _device, const TextureInfo& _info) : D3D12DeviceChild(_device), Texture(_info) {
        DASSERT(false == EnumHasAnyFlag(_info.usage, ETextureUsageFlags::CPU_VISIBLE));
        auto* allocator = _device->GetGpuGlobalAllocator();

        D3D12_RESOURCE_DESC1 resourceDesc;
        resourceDesc.Dimension                       = D3D12EnumTranslation::METoDxResourceDimension(GetDimension());
        resourceDesc.Alignment                       = 0;
        resourceDesc.Width                           = GetWidth();
        resourceDesc.Height                          = GetHeight();
        resourceDesc.DepthOrArraySize                = resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE3D ? GetDepth() : GetNumArray();
        resourceDesc.MipLevels                       = GetNumMips();
        resourceDesc.Format                          = D3D12EnumTranslation::METoDxFormat(GetFormat());
        resourceDesc.SampleDesc.Count                = 1;// todo?
        resourceDesc.SampleDesc.Quality              = 0;
        resourceDesc.Layout                          = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags                           = D3D12EnumTranslation::METoDxTextureResourceFlags(GetUsage());
        resourceDesc.SamplerFeedbackMipRegion.Width  = 0;// todo ?
        resourceDesc.SamplerFeedbackMipRegion.Height = 0;
        resourceDesc.SamplerFeedbackMipRegion.Depth  = 0;

        bool                             is_rtv_dsv = false;
        std::optional<D3D12_CLEAR_VALUE> clearValue;
        if (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
            ASSERT(EnumHasAnyFlag(GetAspectFlags(), ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE));
            clearValue.emplace(CD3DX12_CLEAR_VALUE(resourceDesc.Format, 1.0f, 0));// maybe consider reverseZ
            is_rtv_dsv = true;
        } else if (resourceDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
            ASSERT(EnumHasAnyFlag(GetAspectFlags(), ETextureAspectFlags::COLOR));
            const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            clearValue           = CD3DX12_CLEAR_VALUE(resourceDesc.Format, color);
            is_rtv_dsv           = true;
        }

        const auto allocateInfo = device->Native()->GetResourceAllocationInfo2(0, 1, &resourceDesc, nullptr);
        allocation              = allocator->AllocateTextureHeap("default-texture-name", allocateInfo.SizeInBytes, allocateInfo.Alignment, is_rtv_dsv);
        DX_CHECK_HRESULT(device->Native()->CreatePlacedResource2(
            allocation.alloc->GetHeap(),
            allocation.alloc->GetOffset(),
            &resourceDesc,
            D3D12_BARRIER_LAYOUT_UNDEFINED,// simply use undefined to init, ez for later
            clearValue ? &clearValue.value() : nullptr,
            0,// omit castable format
            nullptr,
            IID_PPV_ARGS(&allocation.resource)));
    }
    D3D12Texture::D3D12Texture(D3D12Device* _device, const TextureInfo& _info, Allocation&& _allocation) : D3D12DeviceChild(_device), Texture(_info), allocation(std::move(_allocation)) {
    }
    D3D12Texture::~D3D12Texture() {
    }
    uint D3D12Texture::QuerySubresourceIndex(uint _mip_level, uint _array_slice, uint _plane_slice) const {
        return D3D12CalcSubresource(_mip_level, _array_slice, _plane_slice, GetNumMips(), GetNumArray());
    }
    void D3D12Texture::Destroy() {
        MoerDelete(this);
    }
    uint D3D12Texture::GetMipByteSize(uint _mip_level) const {
        uint mip_width  = std::max(1u, GetWidth() >> _mip_level);
        uint mip_height = std::max(1u, GetHeight() >> _mip_level);
        uint mip_depth  = std::max(1u, GetDepth() >> _mip_level);
        return mip_width * mip_height * mip_depth * D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetBitsPerUnit(D3D12EnumTranslation::METoDxFormat(GetFormat())) / 8;
    }
    void D3D12Texture::SetName(const std::string_view _name) {
        debug_name.emplace(_name);
        allocation.resource->SetName(StringWiden(_name).c_str());
    }

    namespace D3D12EnumTranslation {

        DXGI_FORMAT METoDxFormat(EPixelFormat _format) {
            // ref https://github.com/doitsujin/dxvk/blob/master/src/dxgi/dxgi_format.cpp
            //     https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/utils.c#L48
            //     https://registry.khronos.org/vulkan/specs/latest/man/html/VkFormat.html
            //     https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format#byte-order--lsb-msb- that's why some vulkan format like R5G6B5 match dxgi format B5G6R5
            //     https://stackoverflow.com/questions/59628956/what-is-the-difference-between-normalized-scaled-and-integer-vkformats

            // currently, not use typeless format
            switch (_format) {
                    // clang-format off
                case PF_UNDEFINED:                       return DXGI_FORMAT_UNKNOWN;
                //case PF_R4G4_UNORM_PACK8:                return DXGI_FORMAT_UNKNOWN;
                //case PF_R4G4B4A4_UNORM_PACK16:           return DXGI_FORMAT_UNKNOWN;
                //case PF_B4G4R4A4_UNORM_PACK16:           return DXGI_FORMAT_UNKNOWN;
                case PF_R5G6B5_UNORM_PACK16:             return DXGI_FORMAT_B5G6R5_UNORM;
                //case PF_B5G6R5_UNORM_PACK16:             return DXGI_FORMAT_UNKNOWN;
                //case PF_R5G5B5A1_UNORM_PACK16:           return DXGI_FORMAT_UNKNOWN;
                //case PF_B5G5R5A1_UNORM_PACK16:           return DXGI_FORMAT_UNKNOWN;
                case PF_A1R5G5B5_UNORM_PACK16:           return DXGI_FORMAT_B5G5R5A1_UNORM;
                case PF_R8_UNORM:                        return DXGI_FORMAT_R8_UNORM;
                case PF_R8_SNORM:                        return DXGI_FORMAT_R8_SNORM;
                //case PF_R8_USCALED:                      return DXGI_FORMAT_UNKNOWN;
                //case PF_R8_SSCALED:                      return DXGI_FORMAT_UNKNOWN;
                case PF_R8_UINT:                         return DXGI_FORMAT_R8_UINT;
                case PF_R8_SINT:                         return DXGI_FORMAT_R8_SINT;
                //case PF_R8_SRGB:                         return DXGI_FORMAT_UNKNOWN;
                case PF_R8G8_UNORM:                      return DXGI_FORMAT_R8G8_UNORM;
                case PF_R8G8_SNORM:                      return DXGI_FORMAT_R8G8_SNORM;
                //case PF_R8G8_USCALED:                    return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8_SSCALED:                    return DXGI_FORMAT_UNKNOWN;
                case PF_R8G8_UINT:                       return DXGI_FORMAT_R8G8_UINT;
                case PF_R8G8_SINT:                       return DXGI_FORMAT_R8G8_SINT;
                //case PF_R8G8_SRGB:                       return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_UNORM:                    return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_SNORM:                    return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_USCALED:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_SSCALED:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_UINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_SINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8_SRGB:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_UNORM:                    return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_SNORM:                    return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_USCALED:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_SSCALED:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_UINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_SINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8_SRGB:                     return DXGI_FORMAT_UNKNOWN;
                case PF_R8G8B8A8_UNORM:                  return DXGI_FORMAT_R8G8B8A8_UNORM;
                case PF_R8G8B8A8_SNORM:                  return DXGI_FORMAT_R8G8B8A8_SNORM;
                //case PF_R8G8B8A8_USCALED:                return DXGI_FORMAT_UNKNOWN;
                //case PF_R8G8B8A8_SSCALED:                return DXGI_FORMAT_UNKNOWN;
                case PF_R8G8B8A8_UINT:                   return DXGI_FORMAT_R8G8B8A8_UINT;
                case PF_R8G8B8A8_SINT:                   return DXGI_FORMAT_R8G8B8A8_SINT;
                case PF_R8G8B8A8_SRGB:                   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                case PF_B8G8R8A8_UNORM:                  return DXGI_FORMAT_B8G8R8A8_UNORM;
                //case PF_B8G8R8A8_SNORM:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8A8_USCALED:                return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8A8_SSCALED:                return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8A8_UINT:                   return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8A8_SINT:                   return DXGI_FORMAT_UNKNOWN;
                //case PF_B8G8R8A8_SRGB:                   return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_UNORM_PACK32:           return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_SNORM_PACK32:           return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_USCALED_PACK32:         return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_SSCALED_PACK32:         return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_UINT_PACK32:            return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_SINT_PACK32:            return DXGI_FORMAT_UNKNOWN;
                //case PF_A8B8G8R8_SRGB_PACK32:            return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_UNORM_PACK32:        return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_SNORM_PACK32:        return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_USCALED_PACK32:      return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_SSCALED_PACK32:      return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_UINT_PACK32:         return DXGI_FORMAT_UNKNOWN;
                //case PF_A2R10G10B10_SINT_PACK32:         return DXGI_FORMAT_UNKNOWN;
                case PF_A2B10G10R10_UNORM_PACK32:        return DXGI_FORMAT_R10G10B10A2_UNORM;
                //case PF_A2B10G10R10_SNORM_PACK32:        return DXGI_FORMAT_UNKNOWN;
                //case PF_A2B10G10R10_USCALED_PACK32:      return DXGI_FORMAT_UNKNOWN;
                //case PF_A2B10G10R10_SSCALED_PACK32:      return DXGI_FORMAT_UNKNOWN;
                case PF_A2B10G10R10_UINT_PACK32:         return DXGI_FORMAT_R10G10B10A2_UINT;
                //case PF_A2B10G10R10_SINT_PACK32:         return DXGI_FORMAT_UNKNOWN;
                case PF_R16_UNORM:                       return DXGI_FORMAT_R16_UNORM;
                case PF_R16_SNORM:                       return DXGI_FORMAT_R16_SNORM;
                //case PF_R16_USCALED:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_R16_SSCALED:                     return DXGI_FORMAT_UNKNOWN;
                case PF_R16_UINT:                        return DXGI_FORMAT_R16_UINT;
                case PF_R16_SINT:                        return DXGI_FORMAT_R16_SINT;
                case PF_R16_SFLOAT:                      return DXGI_FORMAT_R16_FLOAT;
                case PF_R16G16_UNORM:                    return DXGI_FORMAT_R16G16_UNORM;
                case PF_R16G16_SNORM:                    return DXGI_FORMAT_R16G16_SNORM;
                //case PF_R16G16_USCALED:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16_SSCALED:                  return DXGI_FORMAT_UNKNOWN;
                case PF_R16G16_UINT:                     return DXGI_FORMAT_R16G16_UINT;
                case PF_R16G16_SINT:                     return DXGI_FORMAT_R16G16_SINT;
                case PF_R16G16_SFLOAT:                   return DXGI_FORMAT_R16G16_FLOAT;
                //case PF_R16G16B16_UNORM:                 return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_SNORM:                 return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_USCALED:               return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_SSCALED:               return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_UINT:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_SINT:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16_SFLOAT:                return DXGI_FORMAT_UNKNOWN;
                case PF_R16G16B16A16_UNORM:              return DXGI_FORMAT_R16G16B16A16_UNORM;
                case PF_R16G16B16A16_SNORM:              return DXGI_FORMAT_R16G16B16A16_SNORM;
                //case PF_R16G16B16A16_USCALED:            return DXGI_FORMAT_UNKNOWN;
                //case PF_R16G16B16A16_SSCALED:            return DXGI_FORMAT_UNKNOWN;
                case PF_R16G16B16A16_UINT:               return DXGI_FORMAT_R16G16B16A16_UINT;
                case PF_R16G16B16A16_SINT:               return DXGI_FORMAT_R16G16B16A16_SINT;
                case PF_R16G16B16A16_SFLOAT:             return DXGI_FORMAT_R16G16B16A16_FLOAT;
                case PF_R32_UINT:                        return DXGI_FORMAT_R32_UINT;
                case PF_R32_SINT:                        return DXGI_FORMAT_R32_SINT;
                case PF_R32_SFLOAT:                      return DXGI_FORMAT_R32_FLOAT;
                case PF_R32G32_UINT:                     return DXGI_FORMAT_R32G32_UINT;
                case PF_R32G32_SINT:                     return DXGI_FORMAT_R32G32_SINT;
                case PF_R32G32_SFLOAT:                   return DXGI_FORMAT_R32G32_FLOAT;
                case PF_R32G32B32_UINT:                  return DXGI_FORMAT_R32G32B32_UINT;
                case PF_R32G32B32_SINT:                  return DXGI_FORMAT_R32G32B32_SINT;
                case PF_R32G32B32_SFLOAT:                return DXGI_FORMAT_R32G32B32_FLOAT;
                case PF_R32G32B32A32_UINT:               return DXGI_FORMAT_R32G32B32A32_UINT;
                case PF_R32G32B32A32_SINT:               return DXGI_FORMAT_R32G32B32A32_SINT;
                case PF_R32G32B32A32_SFLOAT:             return DXGI_FORMAT_R32G32B32A32_FLOAT;
                //case PF_R64_UINT:                        return DXGI_FORMAT_UNKNOWN;
                //case PF_R64_SINT:                        return DXGI_FORMAT_UNKNOWN;
                //case PF_R64_SFLOAT:                      return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64_UINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64_SINT:                     return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64_SFLOAT:                   return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64_UINT:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64_SINT:                  return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64_SFLOAT:                return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64A64_UINT:               return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64A64_SINT:               return DXGI_FORMAT_UNKNOWN;
                //case PF_R64G64B64A64_SFLOAT:             return DXGI_FORMAT_UNKNOWN;
                case PF_B10G11R11_UFLOAT_PACK32:         return DXGI_FORMAT_R11G11B10_FLOAT; // note, ufloat vs float
                case PF_E5B9G9R9_UFLOAT_PACK32:          return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
                case PF_D16_UNORM:                       return DXGI_FORMAT_D16_UNORM;
                //case PF_X8_D24_UNORM_PACK32:             return DXGI_FORMAT_UNKNOWN;
                case PF_D32_SFLOAT:                      return DXGI_FORMAT_D32_FLOAT;
                //case PF_S8_UINT:                         return DXGI_FORMAT_UNKNOWN;
                //case PF_D16_UNORM_S8_UINT:               return DXGI_FORMAT_UNKNOWN;
                case PF_D24_UNORM_S8_UINT:               return DXGI_FORMAT_D24_UNORM_S8_UINT;
                case PF_D32_SFLOAT_S8_UINT:              return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                //case PF_BC1_RGB_UNORM_BLOCK:             return DXGI_FORMAT_UNKNOWN;
                //case PF_BC1_RGB_SRGB_BLOCK:              return DXGI_FORMAT_UNKNOWN;
                case PF_BC1_RGBA_UNORM_BLOCK:            return DXGI_FORMAT_BC1_UNORM;
                case PF_BC1_RGBA_SRGB_BLOCK:             return DXGI_FORMAT_BC1_UNORM_SRGB;
                case PF_BC2_UNORM_BLOCK:                 return DXGI_FORMAT_BC2_UNORM;
                case PF_BC2_SRGB_BLOCK:                  return DXGI_FORMAT_BC2_UNORM_SRGB;
                case PF_BC3_UNORM_BLOCK:                 return DXGI_FORMAT_BC3_UNORM;
                case PF_BC3_SRGB_BLOCK:                  return DXGI_FORMAT_BC3_UNORM_SRGB;
                case PF_BC4_UNORM_BLOCK:                 return DXGI_FORMAT_BC4_UNORM;
                case PF_BC4_SNORM_BLOCK:                 return DXGI_FORMAT_BC4_SNORM;
                case PF_BC5_UNORM_BLOCK:                 return DXGI_FORMAT_BC5_UNORM;
                case PF_BC5_SNORM_BLOCK:                 return DXGI_FORMAT_BC5_SNORM;
                case PF_BC6H_UFLOAT_BLOCK:               return DXGI_FORMAT_BC6H_UF16;
                case PF_BC6H_SFLOAT_BLOCK:               return DXGI_FORMAT_BC6H_SF16;
                case PF_BC7_UNORM_BLOCK:                 return DXGI_FORMAT_BC7_UNORM;
                case PF_BC7_SRGB_BLOCK:                  return DXGI_FORMAT_BC7_UNORM_SRGB;
                case PF_G8_B8R8_2PLANE_420_UNORM:        return DXGI_FORMAT_NV12;
                //case PF_G8_B8R8_2PLANE_420_UNORM:return DXGI_FORMAT_420_OPAQUE; // alternative to NV12, depends on whether tex layout is standard/non-opaque
                //case PF_G16_B16R16_2PLANE_420_UNORM:return DXGI_FORMAT_P010;  // P010 and P016 are functionally equivalent, 10 means use 10 out of 16 bits
                case PF_G16_B16R16_2PLANE_420_UNORM:     return DXGI_FORMAT_P016;
                case PF_G8B8G8R8_422_UNORM:              return DXGI_FORMAT_YUY2;
                case PF_A4R4G4B4_UNORM_PACK16:           return DXGI_FORMAT_B4G4R4A4_UNORM;
                case PF_R4G4B4A4_UNORM_PACK16:           return DXGI_FORMAT_A4B4G4R4_UNORM;
                // clang-format on
                default:
                    FATAL("Unsupported pixel format: {}", static_cast<uint32_t>(_format));
                    return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_RESOURCE_DIMENSION METoDxResourceDimension(ETextureDimension _dim) {
            switch (_dim) {
                case ETextureDimension::TEX_2D:
                case ETextureDimension::TEX_2D_ARRAY:
                case ETextureDimension::TEX_CUBE:
                case ETextureDimension::TEX_CUBE_ARRAY: return D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                case ETextureDimension::TEX_3D: return D3D12_RESOURCE_DIMENSION::D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                default:
                    FATAL("Unsupported texture dimension: {}", static_cast<uint32_t>(_dim));
                    return D3D12_RESOURCE_DIMENSION_UNKNOWN;
            }
        }

        D3D12_RESOURCE_FLAGS METoDxTextureResourceFlags(ETextureUsageFlags _me_flags) {
            D3D12_RESOURCE_FLAGS ret = D3D12_RESOURCE_FLAG_NONE;

            ASSERT(false == EnumHasAnyFlag(_me_flags, ETextureUsageFlags::CPU_VISIBLE));// won't create texture directly for upload/readback
            //INPUT_ATTACHMENT         = 1 << 4, // no-op
            //TRANSFER_SRC             = 1 << 5, // no-op
            //TRANSFER_DST             = 1 << 6, // no-op
            //SAMPLED                  = 1 << 7, // no-op
            if (EnumHasAnyFlag(_me_flags, ETextureUsageFlags::UNORDERED_ACCESS)) {
                ret |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            if (EnumHasAnyFlag(_me_flags, ETextureUsageFlags::COLOR_ATTACHMENT)) {
                ret |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            //RESOLVE_ATTACHMENT       = 1 << 10, // no-op
            if (EnumHasAnyFlag(_me_flags, ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT)) {
                ret |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                ASSERT(false == EnumHasAnyFlag(_me_flags, ETextureUsageFlags::COLOR_ATTACHMENT));// can't co-exist
                ASSERT(false == EnumHasAnyFlag(_me_flags, ETextureUsageFlags::UNORDERED_ACCESS));
            }

            if (EnumHasAnyFlag(_me_flags,
                               // clang-format off
                  ETextureUsageFlags::TILLING_NONE
                | ETextureUsageFlags::TRANSIENT_ATTACHMENT
                | ETextureUsageFlags::FRAGMENT_DENSITY_MAP
                | ETextureUsageFlags::FRAGMENT_SHADING_RATE_ATTACHMENT
                | ETextureUsageFlags::VIDEO_DECODE
                | ETextureUsageFlags::VIDEO_ENCODE
                | ETextureUsageFlags::SRGB
                | ETextureUsageFlags::PRESENT)) {
                // clang-format on
                LOG_WARNING("Unsupported texture usage flags: {}", static_cast<uint32_t>(_me_flags));
            }
            return ret;
        }

    }// namespace D3D12EnumTranslation

    void D3D12ResourceStateTracker::RecordState(D3D12Texture* texture, ETextureState _state, EPassType _pass_type, bool _is_write) {
        TextureStateDescription desc;
        switch (_state) {
            case ETextureState::UNDEFINED:
                desc = {.layout = D3D12_BARRIER_LAYOUT_UNDEFINED, .sync = D3D12_BARRIER_SYNC_NONE, .access = D3D12_BARRIER_ACCESS_NO_ACCESS};
                break;
            case ETextureState::TRANSFER:
                desc = {.layout = _is_write ? D3D12_BARRIER_LAYOUT_COPY_DEST : D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                        .sync   = D3D12_BARRIER_SYNC_COPY,
                        .access = _is_write ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COPY_SOURCE};
                break;
            case ETextureState::SHADER_RESOURCE:
            case ETextureState::SAMPLE:
                DASSERT(!_is_write);
                DASSERT(_pass_type != EPassType::Copy);
                desc = {.layout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                        .sync   = _pass_type == EPassType::Graphics ? D3D12_BARRIER_SYNC_DRAW : D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        .access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
                break;
            case ETextureState::RENDER_TARGET:
                DASSERT(_is_write);
                DASSERT(_pass_type == EPassType::Graphics);
                desc = {.layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET, .sync = D3D12_BARRIER_SYNC_RENDER_TARGET, .access = D3D12_BARRIER_ACCESS_RENDER_TARGET};
                break;
            case ETextureState::DEPTH_STENCIL:
                DASSERT(_pass_type == EPassType::Graphics);
                desc = {.layout = _is_write ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE : D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ,
                        .sync   = D3D12_BARRIER_SYNC_COPY,
                        .access = _is_write ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE : D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ};
                break;
            case ETextureState::UNORDERED_ACCESS:
                DASSERT(_is_write);
                DASSERT(_pass_type != EPassType::Copy);// NOTE there is a special case for clearUAV commands, buffer is viewed as UAV, but it works like a copy/transfer
                desc = {.layout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                        .sync   = _pass_type == EPassType::Graphics ? D3D12_BARRIER_SYNC_PIXEL_SHADING : D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        .access = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
                break;
            default:
                FATAL("Unsupported texture state: {} {}", static_cast<uint32_t>(_state), static_cast<uint32_t>(_pass_type), _is_write);
        }
        RecordState(texture, desc);
    }

    void D3D12ResourceStateTracker::RecordState(D3D12Texture* texture, TextureStateDescription _required_state) {
        if (auto it = texture_states.find(texture); it != texture_states.end()) {
            auto& state = it->second.after;
            if (_required_state.layout != D3D12_BARRIER_LAYOUT_UNDEFINED && state.layout != _required_state.layout) {
                FATAL("texture in one layer should not have different layouts");
            }
            state.layout = _required_state.layout;
            state.access |= _required_state.access;
            state.sync |= _required_state.sync;
        } else {
            // new added, set the initial

            // TODO consider queue transfer, e.g. state start from SRV
            const TextureStateDescription init{
                .layout = D3D12_BARRIER_LAYOUT_UNDEFINED,
                .sync   = D3D12_BARRIER_SYNC_NONE,
                .access = D3D12_BARRIER_ACCESS_NO_ACCESS,
            };

            texture_states[texture] = TextureState{
                .before  = init,
                .after   = _required_state,
                .initial = init};
        }
        pending_textures.insert(texture);
    }

    void D3D12ResourceStateTracker::RecordState(D3D12Buffer* buffer, EBufferState _state, EPassType _pass_type, bool _is_write) {
        BufferStateDescription desc;
        switch (_state) {
            case EBufferState::UNDEFINED:
                desc = {.sync = D3D12_BARRIER_SYNC_NONE, .access = D3D12_BARRIER_ACCESS_NO_ACCESS};
                break;
            case EBufferState::TRANSFER:
                desc = {.sync = D3D12_BARRIER_SYNC_COPY, .access = _is_write ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COPY_SOURCE};
                break;
            case EBufferState::VERTEX:
                DASSERT(!_is_write);
                DASSERT(_pass_type == EPassType::Graphics);
                desc = {.sync = D3D12_BARRIER_SYNC_VERTEX_SHADING, .access = D3D12_BARRIER_ACCESS_VERTEX_BUFFER};
                break;
            case EBufferState::INDEX:
                DASSERT(!_is_write);
                DASSERT(_pass_type == EPassType::Graphics);
                desc = {.sync = D3D12_BARRIER_SYNC_VERTEX_SHADING, .access = D3D12_BARRIER_ACCESS_INDEX_BUFFER};
                break;
            case EBufferState::INDIRECT:
                DASSERT(!_is_write);// if want to update indirect args, use uav state
                DASSERT(_pass_type != EPassType::Copy);
                desc = {.sync = D3D12_BARRIER_SYNC_EXECUTE_INDIRECT, .access = D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT};
                break;
            case EBufferState::SHADER_RESOURCE:
                DASSERT(!_is_write);
                DASSERT(_pass_type != EPassType::Copy);
                // also possible to use srv in vertex shader, so not only SYNC_PIXEL_SHADING
                desc = {.sync   = _pass_type == EPassType::Graphics ? D3D12_BARRIER_SYNC_DRAW : D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        .access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE};
                break;
            case EBufferState::UNORDERED_ACCESS:
                DASSERT(_is_write);
                DASSERT(_pass_type != EPassType::Copy);// NOTE there is a special case for clearUAV commands, buffer is viewed as UAV, but it works like a copy/transfer
                desc = {.sync   = _pass_type == EPassType::Graphics ? D3D12_BARRIER_SYNC_PIXEL_SHADING : D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        .access = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
                break;
            default:
                FATAL("Unsupported buffer state: {} {}", static_cast<uint32_t>(_state), static_cast<uint32_t>(_pass_type), _is_write);
        }
        RecordState(buffer, desc);
    }

    void D3D12ResourceStateTracker::RecordState(D3D12Buffer* buffer, BufferStateDescription _required_state) {
        if (auto it = buffer_states.find(buffer); it != buffer_states.end()) {
            auto& state = it->second.after;
            state.access |= _required_state.access;
            state.sync |= _required_state.sync;
        } else {
            // new added, set the initial
            buffer_states[buffer] = BufferState{
                .before = {
                    .sync   = D3D12_BARRIER_SYNC_NONE,
                    .access = D3D12_BARRIER_ACCESS_NO_ACCESS,
                },
                .after = _required_state};
        }
        pending_buffers.insert(buffer);
    }

    static bool IsReadOnlyAccess(D3D12_BARRIER_ACCESS access) {
        // clang-format off
        constexpr D3D12_BARRIER_ACCESS kReadOnlySet = D3D12_BARRIER_ACCESS_VERTEX_BUFFER
                                                    | D3D12_BARRIER_ACCESS_INDEX_BUFFER
                                                    | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER
                                                    | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ
                                                    | D3D12_BARRIER_ACCESS_SHADER_RESOURCE
                                                    | D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT
                                                    | D3D12_BARRIER_ACCESS_COPY_SOURCE
                                                    | D3D12_BARRIER_ACCESS_RESOLVE_SOURCE
                                                    | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
        return (access & ~kReadOnlySet) == 0;
        // clang-format on
    }

    void D3D12ResourceStateTracker::ResolveBarriers() {
        for (auto& tex : pending_textures) {
            ASSERT(texture_states.contains(tex));
            auto& texState = texture_states[tex];

            auto& state = texState;
            if (state.before == state.after && IsReadOnlyAccess(state.before.access)) {
                continue;
            }

            texture_barriers.emplace_back(CD3DX12_TEXTURE_BARRIER(
                state.before.sync,
                state.after.sync,
                state.before.access,
                state.after.access,
                state.before.layout,
                state.after.layout,
                tex->Native(),
                CD3DX12_BARRIER_SUBRESOURCE_RANGE(0xffffffff),// all subresources
                D3D12_TEXTURE_BARRIER_FLAG_NONE));

            state.before = state.after;
            state.after  = TextureStateDescription{};
        }
        pending_textures.clear();

        for (auto& buf : pending_buffers) {
            ASSERT(buffer_states.contains(buf));
            auto& state = buffer_states[buf];

            if (state.before == state.after && IsReadOnlyAccess(state.before.access)) {
                continue;
            }

            buffer_barriers.emplace_back(CD3DX12_BUFFER_BARRIER(
                state.before.sync,
                state.after.sync,
                state.before.access,
                state.after.access,
                buf->Native()));

            state.before = state.after;
            state.after  = BufferStateDescription{};
        }
        pending_buffers.clear();

        global_barriers.clear();
    }

    void D3D12ResourceStateTracker::DispatchBarriers(ID3D12GraphicsCommandList7* list) {
        std::vector<D3D12_BARRIER_GROUP> groups;
        groups.reserve(!texture_barriers.empty() + !buffer_barriers.empty() + !global_barriers.empty());
        if (!buffer_barriers.empty())
            groups.emplace_back(CD3DX12_BARRIER_GROUP(buffer_barriers.size(), buffer_barriers.data()));
        if (!texture_barriers.empty())
            groups.emplace_back(CD3DX12_BARRIER_GROUP(texture_barriers.size(), texture_barriers.data()));
        if (!global_barriers.empty())
            groups.emplace_back(CD3DX12_BARRIER_GROUP(global_barriers.size(), global_barriers.data()));
        if (!groups.empty())
            list->Barrier(groups.size(), groups.data());
        texture_barriers.clear();
        buffer_barriers.clear();
        global_barriers.clear();
    }

    void D3D12ResourceStateTracker::RestoreState() {
        for (auto& [tex, state] : texture_states) {
            if (state.initial == state.before)
                continue;
            //if (pending_textures.contains(tex))
            //    continue;// has next/export state, not reset to init // TODO

            texture_barriers.emplace_back(CD3DX12_TEXTURE_BARRIER(
                state.before.sync,
                state.initial.sync,
                state.before.access,
                state.initial.access,
                state.before.layout,
                state.initial.layout,
                tex->Native(),
                CD3DX12_BARRIER_SUBRESOURCE_RANGE(0xffffffff),// all subresources
                D3D12_TEXTURE_BARRIER_FLAG_NONE));

            state.before = std::exchange(state.after, state.initial);
        }

        // maybe not need
        const auto buffer_init_state = BufferStateDescription{.sync = D3D12_BARRIER_SYNC_NONE, .access = D3D12_BARRIER_ACCESS_NO_ACCESS};
        for (auto& [buf, state] : buffer_states) {
            if (buffer_init_state == state.before)
                continue;

            buffer_barriers.emplace_back(CD3DX12_BUFFER_BARRIER(
                state.before.sync,
                buffer_init_state.sync,
                state.before.access,
                buffer_init_state.access,
                buf->Native()));

            state.before = std::exchange(state.after, buffer_init_state);
        }
    }

    void D3D12ResourceStateTracker::Reset() {
        texture_states.clear();
        texture_barriers.clear();
        pending_textures.clear();
        buffer_states.clear();
        buffer_barriers.clear();
        pending_buffers.clear();
        global_barriers.clear();
    }

    void D3D12PipelineState::Destroy() {
        MoerDelete(this);
    }

    D3D12DescriptorHeapBase::D3D12DescriptorHeapBase(D3D12Device* _device, D3D12_DESCRIPTOR_HEAP_TYPE _type, uint32_t _num_descriptors, bool _is_shader_visible)
        : D3D12DeviceChild(_device) {

        ASSERT(_type != D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);
        const bool b_can_visible = _type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || _type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        if (_is_shader_visible) {
            ASSERT(b_can_visible);
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type           = _type;
        heapDesc.NumDescriptors = static_cast<UINT>(_num_descriptors);
        heapDesc.NodeMask       = 0;
        heapDesc.Flags          = _is_shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        DX_CHECK_HRESULT(device->Native()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.ReleaseAndGetAddressOf())));
        start_handle_cpu = heap->GetCPUDescriptorHandleForHeapStart();

        if (_is_shader_visible) {
            start_handle_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        }

        descriptor_size       = device->Native()->GetDescriptorHandleIncrementSize(_type);
        num_total_descriptors = _num_descriptors;
        type                  = _type;
    }

    DescriptorIndex D3D12CpuDescriptorAllocator::Allocate() {
        if (!free_queue.empty()) {
            auto handle = free_queue.front();
            free_queue.pop();
            return handle;
        }
        ASSERT(!IsHeapFull());

        DescriptorIndex ret{num_descriptors_allocated};
        num_descriptors_allocated++;
        return ret;
    }

    void D3D12CpuDescriptorAllocator::Free(DescriptorIndex handle) {
        free_queue.push(handle);
    }

    D3D12GpuDescriptorAllocator::D3D12GpuDescriptorAllocator(D3D12Device* _device, D3D12_DESCRIPTOR_HEAP_TYPE _type, uint32_t _num_descriptors_total, uint32_t _max_descriptors_per_execution)
        : D3D12GpuDescriptorHeap(_device, _type, std::min(_num_descriptors_total, _device->GetFeatureSupport().MaxViewDescriptorHeapSize() / 2)),
          max_descriptors_per_execution(std::min(_max_descriptors_per_execution, GetNumTotalDescriptors() / 3)),
          num_descriptor_chunk(GetNumTotalDescriptors() / max_descriptors_per_execution) {}

    void D3D12GpuDescriptorAllocator::BeginPushDescriptors() {
        uint idx = ready_chunk_indices.Pop();// return 0, if empty. so our valid indices start from 1.
        if (idx) {
            current_chunk_index  = idx;
            current_chunk_offset = 0;
        } else {
            ASSERT(num_used_chunks + 1 <= num_descriptor_chunk);
            current_chunk_index  = ++num_used_chunks;
            current_chunk_offset = 0;
        }
    }

    void D3D12GpuDescriptorAllocator::EndPushDescriptors() {
        ready_chunk_indices.Push(current_chunk_index);
    }

    DescriptorIndex D3D12GpuDescriptorAllocator::Allocate(uint count) {
        uint start = (current_chunk_index - 1) * max_descriptors_per_execution + current_chunk_offset;
        current_chunk_offset += count;
        ASSERT(current_chunk_offset <= max_descriptors_per_execution);
        return {start};
    }

}// namespace Moer::Render