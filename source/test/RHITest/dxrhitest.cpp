
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = MOER_AGILITY_SDK_VERSION;
}
extern "C" {
__declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

#include <filesystem>
// #include <vcruntime_string.h>
#include "Core.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"
#include "math/Constant.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "misc/Traits.h"
#include "rhi/RHI.h"
#include "modules/render/source/rhi/RHIImpl.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"

#include <iostream>
#include "d3dx12_property_format_table.h"

template<typename T, int N>
std::span<Moer::byte> ToSpan(const std::array<T, N>& arr) {
    return std::span<Moer::byte>{(Moer::byte*)arr.data(), arr.size() * sizeof(T)};
}
template<typename T, int N>
std::span<Moer::byte> ToSpan(T (&arr)[N]) {
    return std::span<Moer::byte>((Moer::byte*)arr, N * sizeof(T));
}

int main(int argc, char** argv) {
    using namespace Moer;
    using namespace Moer::Render;

    try {

        auto capturer = CreatePIXCapturer();

        std::filesystem::path path = argv[0];
        path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
        ConfigManager::GetInstance().Init(path);
        //TaskSystem::Init();
        const auto& rhi_config_as_json = ConfigManager::GetInstance().GetRHIConfigAsJSON();

        DeviceInitInfo info{
            .type           = ERHIType::D3D12,
            .name           = "DXRHITest",
            .config_as_json = rhi_config_as_json};
        RenderDevice::Init(std::move(info));
        auto& device = RenderDevice::Get();

        //LOG_INFO("{}", D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetPlaneCount(DXGI_FORMAT_D24_UNORM_S8_UINT));

        auto tex = device.CreateTexture(Extent2D(10, 30), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS, 1);

        auto buf  = device.CreateBuffer<float>(534 + 30 * 256, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
        auto buf2 = device.CreateBuffer<float>(128, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);

        auto   fence     = device.CreateFence();
        auto&  gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
        uint64 timeline  = 0;

        std::array<float, 32 + 30 * 256> a;
        for (int i = 0; i < 5; ++i) a[i] = i + 1;
        for (int i = 0; i < 10; ++i) {
            LOG_INFO("{}", a[i]);
        }
        CommandList list;
        capturer->Begin("D:\\codebase\\repos\\MoerEngine\\target\\bin\\test.wpix");
        //while (timeline < 10) {
        //    if (timeline > 2) fence->Wait(timeline - 2);
        LOG_INFO("dispatch work on {}", ++timeline);
        //list.Barriers(EQueueType::Graphics, EQueueType::Graphics, EPassType::Copy, WriteBuffer{buf->GetView(), EBufferState::TRANSFER});
        list.CopyFrom(ToSpan(a), buf->GetView());
        //list.Barriers(EQueueType::Graphics, EQueueType::Graphics, EPassType::Copy, ReadBuffer{buf->GetView(), EBufferState::TRANSFER});
        //list.Barriers(EQueueType::Graphics, EQueueType::Graphics, EPassType::Copy, WriteBuffer{buf2->GetView(), EBufferState::TRANSFER});
        list.CopyFrom(buf->GetView(0, 5 * sizeof(float)), buf2->GetView(0, 5 * sizeof(float)));
        //list.Barriers(EQueueType::Graphics, EQueueType::Graphics, EPassType::Copy, ReadBuffer{buf2->GetView(), EBufferState::TRANSFER});

        list.CopyFrom(buf2->GetView(0, 5 * sizeof(float)), ToSpan(a).subspan(5 * sizeof(float), 5 * sizeof(float)));

        //list.CopyFrom(ToSpan(a), tex->GetView());
        //list.CopyFrom(tex->GetView(0), buf->GetView(4));
        //list.CopyFrom(buf->GetView(), tex->GetView());
        gfx_queue.Execute(list.Submit().Signal(fence, timeline));
        //}
        gfx_queue.Sync();
        capturer->End();
        LOG_INFO("dispatch work done");

        for (int i = 0; i < 10; ++i) {
            LOG_INFO("{}", a[i]);
        }

    } catch (const std::exception& e) {
        LOG_ERROR("{}", e.what());
    }

    return 0;
}