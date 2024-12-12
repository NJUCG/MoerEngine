#pragma once
#include "rhi/RHI.h"
#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "Core.h"
#include <cassert>
#include "RHIImpl.h"
#include "vulkan/VulkanDevice.h"
#include "d3d12/D3D12Device.h"
#include "shader/ShaderResourceManager.h"

RHI* g_rhi = nullptr;

extern LockFreeQueueBase<RHIResource, false> pending_deletings;
// global shader
void MTest() {

    EnqueueRenderTask([] {
        // do something
        LOG_INFO("Render System Tick");
    });
}

void RHI::RHIFlushPendingDeletes() {
    Moer::Array<RHIResource*> resources_to_delete;

    pending_deletings.PopAll(resources_to_delete);

    uint32_t num_deletes = resources_to_delete.size();

    //test if its ok
    for (uint32_t i = 0; i < num_deletes; ++i) {
        // LOG_INFO("deleted resource type:{}", uint32_t(resources_to_delete[i]->GetResourceType()));
        //resources_to_delete[i]->GetResourceType() == RRT_VIEWPORT ||
        // if (resources_to_delete[i]->GetResourceType() == RRT_GPU_FENCE) {
        //     continue;
        // }
        MoerDelete(resources_to_delete[i]);
    }
#if _DEBUG
    if (num_deletes > 0) {

        LOG_INFO("{} resources to delete", num_deletes);
    }
#endif
}
RHISRVRef RHI::RHICreateBufferSRV(
    RHIBuffer* _resource,
    uint32_t   _stride,
    uint64_t   _byte_size,
    uint64_t   _byte_offset) {
    assert(_resource != nullptr);
    RHIViewRef view = RHICreateBufferView<v_type_buffer_srv>(_resource, _stride, _byte_size, _byte_offset);
    return RHISRVRef(static_cast<RHISRV*>(view.Get()));
};
RHIUAVRef RHI::RHICreateBufferUAV(
    RHIBuffer* _resource,
    uint32_t   stride,
    uint64_t   _byte_size,
    uint64_t   _byte_offset) {

    assert(_resource != nullptr);
    RHIViewRef view = RHICreateBufferView<v_type_buffer_uav>(_resource, stride, _byte_size, _byte_offset);
    return RHIUAVRef(static_cast<RHIUAV*>(view.Get()));
};

RHISRVRef RHI::RHICreateTextureSRV(RHITexture*  _texture,
                                   EPixelFormat _format,
                                   uint32_t     _mip_level,
                                   uint32_t     _mip_levels,
                                   uint32_t     _array_index,
                                   uint32_t     _array_size) {

    assert(_texture != nullptr);
    RHIViewRef view = RHICreateViewInner(
        _texture, GetTextureSRVInfo(_texture, _format, _mip_level, _mip_levels, _array_index, _array_size));
    return RHISRVRef(static_cast<RHISRV*>(view.Get()));
}

RHIUAVRef RHI::RHICreateTextureUAV(RHITexture*  _texture,
                                   EPixelFormat _format,
                                   uint32_t     _mip_level,
                                   uint32_t     _array_index,
                                   uint32_t     _array_size) {
    assert(_texture != nullptr);
    RHIViewRef view = RHICreateViewInner(
        _texture, GetTextureUAVInfo(_texture, _format, _mip_level, _array_index, _array_size));
    return RHIUAVRef(static_cast<RHIUAV*>(view.Get()));
}

RHISRVRef RHI::RHICreateAccelerationStructureSRV(RHIRayTracingTLAS* _tlas) {
    assert(_tlas != nullptr);
    RHIViewRef view = RHICreateViewInner(_tlas, GetAccelerationStructureSRVInfo(_tlas));
    return RHISRVRef(static_cast<RHISRV*>(view.Get()));
}

namespace Moer::Render {
    template<>
    VulkanRHIConfig ResolveConfigAs(const MoerRHIConfigAsJSON& _config_as_json) {
        using std::string;
        assert(_config_as_json["rhi"].get<string>() == "vulkan");

        VulkanRHIConfig config;

        float api = _config_as_json["api_version"].get<float>();
        if (api == 1.f)
            config.api_version = VK_API_VERSION_1_0;
        else if (api == 1.1f)
            config.api_version = VK_API_VERSION_1_1;
        else if (api == 1.2f)
            config.api_version = VK_API_VERSION_1_2;
        else if (api == 1.3f)
            config.api_version = VK_API_VERSION_1_3;
        else {
            LOG_ERROR("Unsupported vulkan api version: {}", api);
            assert(false);
        }

        return config;
    }

     template<>
     D3D12RHIConfig ResolveConfigAs(const MoerRHIConfigAsJSON& _config_as_json) {
        using std::string;
        assert(_config_as_json["rhi"].get<string>() == "d3d12");

         return D3D12RHIConfig();
     }

    RenderDevice& RenderDevice::Get() {
        static RenderDevice device;
        return device;
    }
    void RenderDevice::Init(DeviceInitInfo&& _info) {
        switch (_info.type) {
            case ERHIType::Vulkan:
                Get().impl = std::move(UniquePtr<Impl>(MoerNew(VulkanDevice)(ResolveConfigAs<VulkanRHIConfig>(_info.config_as_json))));
                break;
            case ERHIType::D3D12:
                Get().impl = std::move(UniquePtr<Impl>(MoerNew(D3D12Device)(ResolveConfigAs<D3D12RHIConfig>(_info.config_as_json))));
                //LOG_ERROR("D3D12 is not supported yet");
                break;
        }
        Get().rhi_type = _info.type;
        Get().impl->PostInit();
    }
    void RenderDevice::Dispose() {
        Get().impl.reset();
    }
    CommandQueue& RenderDevice::GetCommandQueue(EQueueType _type) {
        return Get().impl->GetCommandQueue(_type);
    }

    CopyQueue& RenderDevice::GetCopyQueue() {
        return Get().impl->GetCopyQueue();
    }
};// namespace Moer::Render