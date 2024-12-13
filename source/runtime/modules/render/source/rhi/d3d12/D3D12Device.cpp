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

namespace Moer::Render {

static void ThrowIfFailed(HRESULT hr) {
    /*  if (FAILED(hr)) {
        throw HrException(hr);
    }*/
    assert(SUCCEEDED(hr));
}

Moer::Render::D3D12Device::D3D12Device(const D3D12RHIConfig&& _config) {

    UINT dxgiFactoryFlags = 0;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())));

    ComPtr<IDXGIAdapter1> hardwareAdapter;
    GetHardwareAdapter(factory, hardwareAdapter.ReleaseAndGetAddressOf(), true);

    ThrowIfFailed(D3D12CreateDevice(
        hardwareAdapter.Get(),
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));
    ThrowIfFailed(hardwareAdapter.As(&adapter));
    hardwareAdapter = nullptr;

    {
        // d3d12MA

        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice  = device.Get();
        allocatorDesc.pAdapter = adapter.Get();
        // These flags are optional but recommended.
        allocatorDesc.Flags = static_cast<D3D12MA::ALLOCATOR_FLAGS>(// cast make it happy
            D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED | D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED);
            //| D3D12MA::ALLOCATOR_FLAG_SINGLETHREADED);
        ThrowIfFailed(D3D12MA::CreateAllocator(&allocatorDesc, d3d12Allocator.ReleaseAndGetAddressOf()));
    }


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
    return nullptr;
}

BindlessArrayRef D3D12Device::CreateBindlessArray(uint _max_size) {
    return nullptr;
}

FenceRef D3D12Device::CreateFence() {
    return nullptr;
}

RaytracingGeometryRef D3D12Device::CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) {
    return nullptr;
}

RaytracingSceneRef D3D12Device::CreateRaytracingScene() {
    return nullptr;
}

CommandQueue& D3D12Device::GetCommandQueue(EQueueType _type) {
    // TODO: 在此处插入 return 语句
    struct DummyCommandQueue : CommandQueue {
        void              Test();
        virtual void      Wait(WaitEvent _event) {};
        virtual WaitEvent Execute(CmdSubmit&& _submit) { return {}; };
        virtual void      Present(SwapchainRef _swapchain, TextureView _target) {};
        virtual void      Sync()                                                {};
    };
    static DummyCommandQueue queue;
    return queue;
}

CopyQueue& D3D12Device::GetCopyQueue() {
    // TODO: 在此处插入 return 语句
    struct DummyCopyQueue : CopyQueue {
        virtual IOWaitEvt Execute(IOSubmission&& _submit) { return {}; };
        virtual IOWaitEvt Execute(CmdSubmit&& _submit) { return {}; };

        virtual void CopyFrom(BufferView _src, BufferView _dst)        {};
        virtual void CopyFrom(TextureView _src, TextureView _dst)      {};
        virtual void CopyFrom(TextureView _src, BufferView _dst)       {};
        virtual void CopyFrom(BufferView _src, TextureView _dst)       {};
        virtual void CopyFrom(std::span<byte> _data, BufferView _dst)  {};
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
    if (pFactory.As(&factory6)) {
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

} // namespace Moer::Render