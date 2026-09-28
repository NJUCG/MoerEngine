#include "rhi/RHI.h"
#include "Core.h"
#include "PixelFormat.h"
#include "RHIImpl.h"
#if defined(_WIN32)
#include "d3d12/D3D12Device.h"
#endif
#if defined(__APPLE__)
#include "metal/MetalDevice.h"
#endif
#include "log/LogSystem.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIThreadHeartbeat.h"
#include "shader/ShaderResourceManager.h"
#if !defined(__APPLE__)
#include "vulkan/VulkanDevice.h"
#endif

#include <stdexcept>

namespace Moer::Render {
namespace {

class RHIHeartbeatStopGuard final {
public:
    explicit RHIHeartbeatStopGuard(RHIThreadHeartbeat& _heartbeat) noexcept :
        heartbeat(_heartbeat) {}

    ~RHIHeartbeatStopGuard() noexcept {
        if (armed) {
            heartbeat.Stop();
        }
    }

    void Dismiss() noexcept {
        armed = false;
    }

private:
    RHIThreadHeartbeat& heartbeat;
    bool                armed{true};
};

} // namespace

#if !defined(__APPLE__)
template<>
VulkanRHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
    using std::string;
    MOER_ASSERT(
        _info.rhi_type == ERHIType::Vulkan,
        "ResolveConfigAs<VulkanRHIConfig> requires Vulkan"
    );

    VulkanRHIConfig config;
    config.rhi_thread                   = _info.rhi_thread;
    config.rhi_bypass                   = _info.rhi_bypass;
    config.thread_profile_logging       = _info.thread_profile_logging;
    config.parallel_recording           = _info.parallel_recording;
    config.parallel_record_workers      = _info.parallel_record_workers;
    config.parallel_record_verify       = _info.parallel_record_verify;
    config.parallel_record_profile      = _info.parallel_record_profile;
    config.parallel_record_min_work_units_per_job =
        _info.parallel_record_min_work_units_per_job;
    config.parallel_record_worker_throw_trigger =
        _info.parallel_record_worker_throw_trigger;
    config.present_submit_fault_trigger = _info.vulkan_present_submit_fault_trigger;

    std::string_view api = _info.rhi_api_version;

    api = api.substr(0, 3); // 1.0, 1.1, 1.2, 1.3
    if (api == "1.0")
        config.api_version = VK_API_VERSION_1_0;
    else if (api == "1.1")
        config.api_version = VK_API_VERSION_1_1;
    else if (api == "1.2")
        config.api_version = VK_API_VERSION_1_2;
    else if (api == "1.3")
        config.api_version = VK_API_VERSION_1_3;
    else {
        LOG_ERROR("Unsupported vulkan api version: {}", api);
        MOER_ASSERT(false, "Unsupported Vulkan API version: {}", api);
    }

    return config;
}
#endif

#if defined(_WIN32)
template<>
D3D12RHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
    MOER_ASSERT(
        _info.rhi_type == ERHIType::D3D12,
        "ResolveConfigAs<D3D12RHIConfig> requires D3D12"
    );

    D3D12RHIConfig config;

    config.force_sync = true; //       _config_as_json.value("force_sync", false);

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
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Start(
        RHIThreadHeartbeatConfig{
            .enabled = _info.rhi_heartbeat_enabled,
            .stall_timeout_ms = _info.rhi_heartbeat_stall_timeout_ms,
            .poll_interval_ms = _info.rhi_heartbeat_poll_interval_ms,
        }
    );
    RHIHeartbeatStopGuard heartbeat_stop_guard(heartbeat);
    try {
        switch (_info.rhi_type) {
            case ERHIType::Vulkan:
#if defined(__APPLE__)
                throw std::runtime_error("Vulkan backend is not built on macOS; select Metal when available");
#else
                Get().impl =
                    std::move(UniquePtr<Impl>(MoerNew(VulkanDevice)(ResolveConfigAs<VulkanRHIConfig>(_info))));
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
        heartbeat_stop_guard.Dismiss();
    } catch (...) {
        RHIExecutor::ShutDown();
        Get().impl.reset();
        throw;
    }
}
void RenderDevice::Dispose() {
    RHIHeartbeatStopGuard heartbeat_stop_guard(
        RHIThreadHeartbeat::Get()
    );
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
