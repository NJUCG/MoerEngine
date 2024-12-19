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
#include <spirv_reflect.h>
#include <variant>
#include <config.h>
#include <platform/Platform.h>
#include "../vulkan/RHICmdReorderer.h"

namespace Moer::Render {

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

        gpu_global_allocator = MakeUnique<D3D12GpuGlobalAllocator>(this);
        gfx_queue            = MakeUnique<D3D12GraphicsCommandQueue>(this, EQueueType::Graphics);
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

    PipelineHandle D3D12Device::CreatePipeline(PipelineShaderInfo&& _shaders) {
        return PipelineHandle();
    }

    TextureRef D3D12Device::CreateTexture(std::string_view _name, ETextureDimension _dimension, Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint _array_size) {
        return nullptr;
    }

    BufferRef D3D12Device::CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) {
        BufferInfo info{_element_cnt, _byte_stride, _usage};
        return BufferRef{MoerNew(D3D12Buffer)(this, info)};
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
            void              Test();
            virtual void      Wait(WaitEvent _event) {};
            virtual WaitEvent Execute(CmdSubmit&& _submit) { return {}; };
            virtual void      Present(SwapchainRef _swapchain, TextureView _target) {};
            virtual void      Sync() {};
        };
        static DummyCommandQueue queue;
        return queue;
    }

    CopyQueue& D3D12Device::GetCopyQueue() {
        // TODO: 在此处插入 return 语句
        struct DummyCopyQueue : CopyQueue {
            virtual IOWaitEvt Execute(IOSubmission&& _submit) { return {}; };
            virtual IOWaitEvt Execute(CmdSubmit&& _submit) { return {}; };

            virtual void CopyFrom(BufferView _src, BufferView _dst) {};
            virtual void CopyFrom(TextureView _src, TextureView _dst) {};
            virtual void CopyFrom(TextureView _src, BufferView _dst) {};
            virtual void CopyFrom(BufferView _src, TextureView _dst) {};
            virtual void CopyFrom(std::span<byte> _data, BufferView _dst) {};
            virtual void CopyFrom(std::span<byte> _data, TextureView _dst) {};

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

        uint64 align = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        if (EnumHasAnyFlag(_info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE | EBufferUsageFlags::ACCELERATION_STRUCTURE_SCRATCH | EBufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT))
            align = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        allocation = allocator->AllocateBufferHeap("default-buffer-name", GetByteSize(), align, D3D12_HEAP_TYPE_DEFAULT);

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

    Allocation D3D12GpuGlobalAllocator::AllocateBufferHeap(std::string_view _name, uint64 _byte_size, uint64 _align, D3D12_HEAP_TYPE _heap_type) {
        D3D12MA::ALLOCATION_DESC desc;
        desc.HeapType       = _heap_type;
        desc.Flags          = D3D12MA::ALLOCATION_FLAGS::ALLOCATION_FLAG_STRATEGY_BEST_FIT;
        desc.ExtraHeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        desc.CustomPool     = nullptr;
        D3D12_RESOURCE_ALLOCATION_INFO info;
        DASSERT(IsPowerOfTwo(_align));
        info.Alignment   = _align;
        info.SizeInBytes = AlignUpToPowerOfTwo(_byte_size, _align);

        Allocation alloc;
        DX_CHECK_HRESULT(d3d12Allocator->AllocateMemory(&desc, &info, &alloc.alloc));
        alloc.alloc->SetPrivateData(device);// sometime useful
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
                        LOG_INFO("visit event: allocator, {}", fence_value);
                        AtomicMaximum(last_completed_fence_value, fence_value);// break the 'Sync' condition, later
                    },
                    [&fence_value](const Array<std::function<void()>>& funcs) {
                        LOG_INFO("visit event: callback, {}", fence_value);
                        for (auto&& f : funcs) f();
                    },
                    [&fence_value](SignalEvent e) {
                        LOG_INFO("visit event: signal, {}", fence_value);
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

        void Visit(const UploadBufferCmd& _cmd) {}

        void Visit(const CopyBackBufferCmd& _cmd) {}

        void Visit(const CopyBufferCmd& _cmd) {
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
            auto data_span = _cmd.Data();
            auto tmp_buffer = allocator.AllocateUploadBuffer(_cmd.ByteSize(), 256);// ? not sure alignment
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
            D3D12Buffer* buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            cmd_list.CopyBuffer(tmp_buffer.buffer, buffer, _cmd.ByteSize(), tmp_buffer.byte_offset, _cmd.Offset());

            // FIX state barrier!!
            {
                D3D12_BUFFER_BARRIER BufBarriers[] = {
                    CD3DX12_BUFFER_BARRIER(
                        D3D12_BARRIER_SYNC_COPY,
                        D3D12_BARRIER_SYNC_COPY,
                        D3D12_BARRIER_ACCESS_COPY_DEST,
                        D3D12_BARRIER_ACCESS_COPY_SOURCE,
                        buffer->Native())};
                D3D12_BARRIER_GROUP BufBarrierGroups[] = {
                    CD3DX12_BARRIER_GROUP(1, BufBarriers)};
                cmd_list.Native()->Barrier(1, BufBarrierGroups);
            }
        }

        void Visit(const CopyBackBufferCmd& _cmd) {
            auto         tmp_buffer = allocator.AllocateReadbackBuffer(_cmd.ByteSize(), 256);// ? not sure alignment
            D3D12Buffer* src_buffer = reinterpret_cast<D3D12Buffer*>(_cmd.Handle());
            // FIX state barrier!!

            {
                D3D12_BUFFER_BARRIER BufBarriers[] = {
                    CD3DX12_BUFFER_BARRIER(
                        D3D12_BARRIER_SYNC_COPY,
                        D3D12_BARRIER_SYNC_COPY,
                        D3D12_BARRIER_ACCESS_COPY_DEST,
                        D3D12_BARRIER_ACCESS_COPY_SOURCE,
                        src_buffer->Native())};
                D3D12_BARRIER_GROUP BufBarrierGroups[] = {
                    CD3DX12_BARRIER_GROUP(1, BufBarriers)};
                cmd_list.Native()->Barrier(1, BufBarrierGroups);
            }

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
        : D3D12DeviceChild(_device),
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
            //D3D12ResourceStateTracker     tracker;
            //allocator.Native()->Reset();
            for (const auto& cmd : _submit.cmds) {// todo reorder
                //preprocess_visitor.Visit(cmd);

                //tracker.ResolveBarrier();
                //tracker.DispatchBarrier(cmd_list);

                cmd_visitor.VisitCmd(cmd.get());
            }
            {
                //tracker.RestoreState();
                //tracker.DispatchBarriers(cmd_list);
                //tracker.Reset();
            }
        }
        cmd_list->End();

        const uint64 next_fence_value = ++last_submitted_fence_value;

        allocator->AddOnComplete([next_fence_value] {
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
        // allocate large upload buffer
        const auto                 heap_prop    = CD3DX12_HEAP_PROPERTIES(_heap_type);
        const D3D12_RESOURCE_DESC1 resourceDesc = CD3DX12_RESOURCE_DESC1::Buffer(_byte_size);

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
        buffer->Native()->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
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
        buffer->Native()->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
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
        underlying_buffer->Native()->Map(0, nullptr, reinterpret_cast<void**>(&ptr_mapped));
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
}// namespace Moer::Render