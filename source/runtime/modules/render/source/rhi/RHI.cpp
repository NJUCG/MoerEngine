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

namespace Moer::Render {

    template<>
    VulkanRHIConfig ResolveConfigAs(const DeviceInitInfo& _info) {
        using std::string;
        assert(_info.rhi == "vulkan");

        VulkanRHIConfig config;

        std::string_view api = _info.rhi_api_version;

        api = api.substr(0, 3);// 1.0, 1.1, 1.2, 1.3
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
        assert(_info.rhi == "d3d12");

        D3D12RHIConfig config;

        config.force_sync = true;//       _config_as_json.value("force_sync", false);

        return config;
    }

    RenderDevice& RenderDevice::Get() {
        static RenderDevice device;
        return device;
    }
    void RenderDevice::Init(DeviceInitInfo&& _info) {
        switch (_info.type) {
            case ERHIType::Vulkan:
                Get().impl = std::move(UniquePtr<Impl>(MoerNew(VulkanDevice)(ResolveConfigAs<VulkanRHIConfig>(_info))));
                break;
            case ERHIType::D3D12:
                Get().impl = std::move(UniquePtr<Impl>(MoerNew(D3D12Device)(ResolveConfigAs<D3D12RHIConfig>(_info))));
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