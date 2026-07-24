#include "rhi/RHI.h"
#include "Core.h"
#include "PixelFormat.h"
#include "RHIImpl.h"
#include "d3d12/D3D12Device.h"
#include "log/LogSystem.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "shader/ShaderResourceManager.h"
#include "vulkan/VulkanDevice.h"
#include <cassert>

namespace Moer::Render {

template<>
VulkanRHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
    using std::string;
    assert(_info.rhi_type == ERHIType::Vulkan);

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
        assert(false);
    }

    return config;
}

template<>
D3D12RHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
    assert(_info.rhi_type == ERHIType::D3D12);

    D3D12RHIConfig config;

    config.force_sync = true; //       _config_as_json.value("force_sync", false);

    return config;
}

RenderDevice& RenderDevice::Get() {
    static RenderDevice device;
    return device;
}
void RenderDevice::Init(DeviceInitInfo&& _info) {
    switch (_info.rhi_type) {
        case ERHIType::Vulkan:
            Get().impl =
                std::move(UniquePtr<Impl>(MoerNew(VulkanDevice)(ResolveConfigAs<VulkanRHIConfig>(_info))));
            break;
        case ERHIType::D3D12:
            Get().impl =
                std::move(UniquePtr<Impl>(MoerNew(D3D12Device)(ResolveConfigAs<D3D12RHIConfig>(_info))));
            //LOG_ERROR("D3D12 is not supported yet");
            break;
    }
    Get().rhi_type = _info.rhi_type;
    RHIExecutor::StartUp();
    Get().impl->PostInit();
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
bool IsPixelFormatBC(EPixelFormat _format) {
    switch (_format) {
        case PF_BC1_RGB_UNORM_BLOCK:
            return true;
        case PF_BC1_RGB_SRGB_BLOCK:
            return true;
        case PF_BC1_RGBA_UNORM_BLOCK:
            return true;
        case PF_BC1_RGBA_SRGB_BLOCK:
            return true;
        case PF_BC2_UNORM_BLOCK:
            return true;
        case PF_BC2_SRGB_BLOCK:
            return true;
        case PF_BC3_UNORM_BLOCK:
            return true;
        case PF_BC3_SRGB_BLOCK:
            return true;
        case PF_BC4_UNORM_BLOCK:
            return true;
        case PF_BC4_SNORM_BLOCK:
            return true;
        case PF_BC5_UNORM_BLOCK:
            return true;
        case PF_BC5_SNORM_BLOCK:
            return true;
        case PF_BC6H_UFLOAT_BLOCK:
            return true;
        case PF_BC6H_SFLOAT_BLOCK:
            return true;
        case PF_BC7_UNORM_BLOCK:
            return true;
        case PF_BC7_SRGB_BLOCK:
            return true;
    }
    return false;
}
uint64 GetSizeFromImageFormat(EPixelFormat _format, const uint3 _size) {
    return GetSizeFromPixelFormat(_format, _size);
}

uint64 GetByteFromPixelFormat(EPixelFormat format) {
    if (IsPixelFormatBC(format)) {
        assert(false && "BC format does not have fixed byte per pixel");
    }
    switch (format) {
        case PF_R8G8B8A8_SRGB:
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_UINT:
        case PF_R8G8B8A8_SNORM:
        case PF_R8G8B8A8_SINT:
            return 4;
            break;
        case PF_R32G32B32A32_SFLOAT:
        case PF_R32G32B32A32_UINT:
        case PF_R32G32B32A32_SINT:
            return 16;
            break;
        case PF_R32G32_SFLOAT:
        case PF_R32G32_UINT:
        case PF_R32G32_SINT:
            return 8;
            break;
        case PF_R32_SFLOAT:
        case PF_R32_UINT:
        case PF_R32_SINT:
            return 4;
            break;
        case PF_R16G16B16A16_SFLOAT:
        case PF_R16G16B16A16_UNORM:
        case PF_R16G16B16A16_UINT:
        case PF_R16G16B16A16_SNORM:
        case PF_R16G16B16A16_SINT:
            return 8;
            break;
        case PF_R16G16_SFLOAT:
        case PF_R16G16_UNORM:
        case PF_R16G16_UINT:
        case PF_R16G16_SNORM:
        case PF_R16G16_SINT:
            return 4;
            break;
        case PF_R16_SFLOAT:
        case PF_R16_UNORM:
        case PF_R16_UINT:
        case PF_R16_SNORM:
        case PF_R16_SINT:
            return 2;
            break;
        case PF_R8G8B8_SRGB:
        case PF_R8G8B8_UNORM:
        case PF_R8G8B8_UINT:
        case PF_R8G8B8_SNORM:
        case PF_R8G8B8_SINT:
            return 3;
            break;
        case PF_R8G8_SRGB:
        case PF_R8G8_UNORM:
        case PF_R8G8_UINT:
        case PF_R8G8_SNORM:
        case PF_R8G8_SINT:
            return 2;
            break;
        case PF_R8_SRGB:
        case PF_R8_UNORM:
        case PF_R8_UINT:
        case PF_R8_SNORM:
        case PF_R8_SINT:
            return 1;
            break;
        default:
            assert(false && "not support format");
    }
    return 0;
}

uint64 GetChannelFromPixelFormat(EPixelFormat format) {
    if (IsPixelFormatBC(format)) {
        assert(false && "BC format has no fixed channel count");
    }
    switch (format) {
        case PF_R8G8B8A8_SRGB:
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_UINT:
        case PF_R8G8B8A8_SNORM:
        case PF_R8G8B8A8_SINT:
            return 4;
            break;
        case PF_R32G32B32A32_SFLOAT:
        case PF_R32G32B32A32_UINT:
        case PF_R32G32B32A32_SINT:
            return 4;
            break;
        case PF_R32G32_SFLOAT:
        case PF_R32G32_UINT:
        case PF_R32G32_SINT:
            return 2;
            break;
        case PF_R32_SFLOAT:
        case PF_R32_UINT:
        case PF_R32_SINT:
            return 1;
            break;
        case PF_R16G16B16A16_SFLOAT:
        case PF_R16G16B16A16_UNORM:
        case PF_R16G16B16A16_UINT:
        case PF_R16G16B16A16_SNORM:
        case PF_R16G16B16A16_SINT:
            return 4;
            break;
        case PF_R16G16_SFLOAT:
        case PF_R16G16_UNORM:
        case PF_R16G16_UINT:
        case PF_R16G16_SNORM:
        case PF_R16G16_SINT:
            return 2;
            break;
        case PF_R16_SFLOAT:
        case PF_R16_UNORM:
        case PF_R16_UINT:
        case PF_R16_SNORM:
        case PF_R16_SINT:
            return 1;
            break;
        case PF_R8G8B8_SRGB:
        case PF_R8G8B8_UNORM:
        case PF_R8G8B8_UINT:
        case PF_R8G8B8_SNORM:
        case PF_R8G8B8_SINT:
            return 3;
            break;
        case PF_R8G8_SRGB:
        case PF_R8G8_UNORM:
        case PF_R8G8_UINT:
        case PF_R8G8_SNORM:
        case PF_R8G8_SINT:
            return 2;
            break;
        case PF_R8_SRGB:
        case PF_R8_UNORM:
        case PF_R8_UINT:
        case PF_R8_SNORM:
        case PF_R8_SINT:
            return 1;
            break;
        default:
            assert(false && "not support format");
    }
    return 0;
}

uint64 GetSizeFromPixelFormat(EPixelFormat format, const uint3 size) {
    if (IsPixelFormatBC(format)) {
        uint64 block_width  = (size.x + 3) / 4;
        uint64 block_height = (size.y + 3) / 4;
        uint64 block_cnt    = block_width * block_height * std::max(1u, size.z);

        switch (format) {
            case PF_BC1_RGB_UNORM_BLOCK:
            case PF_BC1_RGBA_UNORM_BLOCK:
            case PF_BC1_RGB_SRGB_BLOCK:
            case PF_BC1_RGBA_SRGB_BLOCK:
                return block_cnt * 8;
                break;
            case PF_BC2_UNORM_BLOCK:
            case PF_BC2_SRGB_BLOCK:
            case PF_BC3_UNORM_BLOCK:
            case PF_BC3_SRGB_BLOCK:
                return block_cnt * 16;
                break;
            case PF_BC4_UNORM_BLOCK:
            case PF_BC4_SNORM_BLOCK:
                return block_cnt * 8;
                break;
            case PF_BC5_UNORM_BLOCK:
            case PF_BC5_SNORM_BLOCK:
            case PF_BC6H_UFLOAT_BLOCK:
            case PF_BC6H_SFLOAT_BLOCK:
            case PF_BC7_UNORM_BLOCK:
            case PF_BC7_SRGB_BLOCK:
                return block_cnt * 16;
                break;
            default:
                assert(false && "not support format");
        }
    }
    return GetByteFromPixelFormat(format) * size.x * size.y * size.z;
}

}; // namespace Moer::Render
