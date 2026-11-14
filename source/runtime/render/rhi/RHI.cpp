#include "rhi/RHI.h"
#include "Core.h"
#include "PixelFormat.h"
#include "RHIImpl.h"
#if defined(_WIN32)
#include "rhi/backends/d3d12/D3D12Device.h"
#endif
#if defined(__APPLE__)
#include "rhi/backends/metal/MetalDevice.h"
#endif
#include "log/LogSystem.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/executor/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#if !defined(__APPLE__)
#include "rhi/backends/vulkan/VulkanDevice.h"
#endif

#include <stdexcept>

namespace Moer::Render {

#if defined(_WIN32)
template<>
D3D12RHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
    MOER_ASSERT(
        _info.rhi_type == ERHIType::D3D12,
        "ResolveConfigAs<D3D12RHIConfig> requires D3D12"
    );

    D3D12RHIConfig config;

    config.force_sync = false;

    return config;
}
#endif

RenderDevice& RenderDevice::Get() {
    static RenderDevice device;
    return device;
}
bool RenderDevice::IsInitialized() {
    return Get().impl != nullptr;
}
void RenderDevice::Init(DeviceInitInfo&& _info) {
    try {
        switch (_info.rhi_type) {
            case ERHIType::Vulkan:
#if defined(__APPLE__)
                throw std::runtime_error("Vulkan backend is not built on macOS; select Metal when available");
#else
                if (_info.rhi_api_version != "1.3") {
                    throw std::invalid_argument("The Vulkan backend only supports API version 1.3");
                }
                Get().impl =
                    std::move(UniquePtr<Impl>(MoerNew(VulkanDevice)()));
                break;
#endif
#if defined(_WIN32)
            case ERHIType::D3D12:
                Get().impl =
                    std::move(UniquePtr<Impl>(MoerNew(D3D12Device)(ResolveConfigAs<D3D12RHIConfig>(_info))));
                //LOG_ERROR("D3D12 is not supported yet");
                break;
#else
            case ERHIType::D3D12:
                throw std::runtime_error("D3D12 backend is only available on Windows");
#endif
            case ERHIType::Metal:
#if defined(__APPLE__)
                Get().impl = UniquePtr<Impl>(MoerNew(MetalDevice)());
                break;
#else
                throw std::runtime_error("Metal backend is only available on macOS");
#endif
        }
        Get().rhi_type = _info.rhi_type;
        RHIExecutor::StartUp(_info.submission_batch_window);
        Get().impl->PostInit();
    } catch (...) {
        RHIExecutor::ShutDown();
        Get().impl.reset();
        throw;
    }
}
void RenderDevice::Dispose() {
    RHIExecutor::ShutDown();
    // Pooled RHI objects must die while the backend implementation and its
    // allocators are still alive. In-flight owners have already drained with
    // the executor; Reset only releases the pool's final cache references.
    RenderGraphResourcePool::Global().Reset();
    Get().impl.reset();
}
CommandQueue& RenderDevice::GetCommandQueue(EQueueType _type) {
    return Get().impl->GetCommandQueue(_type);
}

CopyQueue& RenderDevice::GetCopyQueue() {
    return Get().impl->GetCopyQueue();
}

}; // namespace Moer::Render
