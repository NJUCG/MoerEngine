
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
#include "shader/ShaderCompiler.h"
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

    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);

    try {

        auto capturer = CreatePIXCapturer();

        //TaskSystem::Init();
        //const auto& rhi_config_as_json = ConfigManager::GetInstance().GetConfig();

        DeviceInitInfo info{
            .type           = ERHIType::D3D12,
            .name           = "DXRHITest",
            .rhi  = "d3d12"};
        RenderDevice::Init(std::move(info));
        auto& device = RenderDevice::Get();

        //LOG_INFO("{}", D3D12_PROPERTY_LAYOUT_FORMAT_TABLE::GetPlaneCount(DXGI_FORMAT_D24_UNORM_S8_UINT));

        auto tex = device.CreateTexture(Extent2D(10, 30), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS, 1);

        auto buf  = device.CreateBuffer<float>("a", 534 + 30 * 256, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
        auto buf2 = device.CreateBuffer<float>("b", 128, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);

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

    /*

    // cpp
    auto bdls       = device.CreateBindlessArray();
    auto other_tex  = ...;
    auto tex        = ...;
    auto tex_handle = bdls.AllocateTexture(tex);
    auto pipeline   = ...;

    PushConstantStruct param;
    param.tex_handle = tex_handle;

    cmd_list.Gfx(pipeline, other_tex, bdls, param).Draw(...);

    // shader
    struct PushConstantStruct {
        uint32_t tex_handle;
    };
    ConstantBuffer<PushConstantStruct> constants;

    auto       real_idx_in_heap = g__array_114514_bdls[constants.tex_handle];
    Texture2D  tex              = gTexture2D_114514_bdls[real_idx_in_heap];

    */
    ShaderCompilerEnvironment env{};

    //ShaderCompilerInput       input{
    //          .target_info               = ShaderTargetInfo(ST_COMPUTE, SP_WIN_D3D_SM6),
    //          .mutation_id               = 0,
    //          .entry_point               = "CSMain",
    //          .relative_source_file_path = "test/FillArg.hlsl",
    //          .shader_name               = "test/FillArg.hlsl",
    //          .environment               = env};
    //ShaderCompilerInput input{
    //    .target_info               = ShaderTargetInfo(ST_FRAGMENT, SP_VULKAN_SM6),
    //    .mutation_id               = 0,
    //    .entry_point               = "main",
    //    .relative_source_file_path = "test/BasicFragConstant.hlsl",
    //    .shader_name               = "test/BasicFragConstant.hlsl",
    //    .environment               = env};

    //ShaderCompiler::Init();
    //auto output = ShaderCompiler::Compile(input);

    return 0;
}