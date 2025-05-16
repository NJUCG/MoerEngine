
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
#include "shader/ShaderPipeline.h"
#include "window/WindowContext.h"
#include "misc/Timer.h"

constexpr uint32_t kNumBufArr = 7;
constexpr uint32_t kNumTexArr = 9;

namespace Moer::Render {

    class TestReadPipeline : public ComputePipeline {
    public:
        DEFINE_COMPUTE_PIPELINE_CLASS(TestReadPipeline);

        DEFINE_SHADER_BUFFER(buf);

        DEFINE_SHADER_ARGS(buf);
    };

    struct S0 {
        uint   a;
        uint   b;
        uint   tex_handle;
        uint   buf_handle;
        float4 x;
    };
    struct S1 {
        uint a;
    };

    class TestComputePipeline : public ComputePipeline {
    public:
        DEFINE_COMPUTE_PIPELINE_CLASS(TestComputePipeline);

        DEFINE_SHADER_CONSTANT_STRUCT(S0, cb0);
        //DEFINE_SHADER_BUFFER(cb0);
        DEFINE_SHADER_BUFFER(cb1);
        DEFINE_SHADER_BUFFER(sb0);
        DEFINE_SHADER_BUFFER(sb1);
        DEFINE_SHADER_BUFFER(rb0);
        DEFINE_SHADER_BUFFER(rb1);
        DEFINE_SHADER_BUFFER(tb0);
        DEFINE_SHADER_BUFFER(tb1);
        DEFINE_SHADER_BUFFER_ARRAY(buf_arr, kNumBufArr);
        //DEFINE_SHADER_TEX(t0);
        //DEFINE_SHADER_TEX(t1);
        DEFINE_SHADER_TEX(t2);
        DEFINE_SHADER_TEX(t3);
        DEFINE_SHADER_TEX(t4);
        DEFINE_SHADER_TEX(t5);
        DEFINE_SHADER_TEX(t6);
        DEFINE_SHADER_TEX_ARRAY(tex_arr, kNumTexArr);
        //DEFINE_SHADER_TEX(rwt0);
        //DEFINE_SHADER_TEX(rwt1);
        DEFINE_SHADER_TEX(rwt2);
        DEFINE_SHADER_TEX(rwt3);
        DEFINE_SHADER_TEX(rwt4);
        DEFINE_SHADER_SAMPLER(s0);

        DEFINE_SHADER_BINDLESS_ARRAY(bdls);

        DEFINE_SHADER_ARGS(cb0, cb1, sb0, sb1, rb0, rb1, tb0, tb1, buf_arr, t2, t3, t4, t5, t6, tex_arr, rwt2, rwt3, rwt4, s0, bdls);
    };

}// namespace Moer::Render

template<typename T, int N>
std::span<Moer::byte> ToSpan(const std::array<T, N>& arr) {
    return std::span<Moer::byte>{(Moer::byte*)arr.data(), arr.size() * sizeof(T)};
}
template<typename T, int N>
std::span<Moer::byte> ToSpan(T (&arr)[N]) {
    return std::span<Moer::byte>((Moer::byte*)arr, N * sizeof(T));
}
template<typename T>
std::span<Moer::byte> ToSpan(const std::vector<T>& arr) {
    return std::span<Moer::byte>{(Moer::byte*)arr.data(), arr.size() * sizeof(T)};
}
template<typename T>
    requires std::is_standard_layout_v<T>
std::span<Moer::byte> ToSpan(const T& x) {
    return std::span<Moer::byte>{(Moer::byte*)&x, sizeof(T)};
}

void TestBasic() {
    using namespace Moer;
    using namespace Moer::Render;

    auto& device = RenderDevice::Get();

    auto      cb0 = device.CreateBuffer<Moer::byte>("cb0", sizeof(S0), EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST);
    auto      cb1 = device.CreateBuffer<Moer::byte>("cb1", sizeof(S1), EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST);
    auto      sb0 = device.CreateBuffer<float4>("sb0", 1024, EBufferUsageFlags::TRANSFER_DST);
    auto      sb1 = device.CreateBuffer<S1>("sb1", 128, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
    auto      rb0 = device.CreateBuffer<uint>("rb0", 1024, EBufferUsageFlags::TRANSFER_DST);
    auto      rb1 = device.CreateBuffer<float>("rb1", 128, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
    auto      tb0 = device.CreateBuffer<float>("tb0", 1024, EBufferUsageFlags::TRANSFER_DST);
    auto      tb1 = device.CreateBuffer<float>("tb1", 128, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
    BufferRef buf_arr[kNumBufArr];
    for (int i = 0; i < kNumBufArr; ++i) {
        buf_arr[i] = device.CreateBuffer<float>(std::format("buf_array_{}", i), 9, EBufferUsageFlags::TRANSFER_DST);
    }

    // we not really support texture1d... see 'ETextureDimension' (start from 2D
    //auto t0 = device.CreateTexture("t0", Extent2D(2, 1), EPixelFormat::PF_R16G16B16A16_UNORM, ETextureUsageFlags::TRANSFER_DST);
    //auto t1 = device.CreateTexture("t1", Extent2D(2, 1), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST, 1, 2);
    auto t2 = device.CreateTexture("t2", Extent2D(8, 8), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST, 3);
    auto t3 = device.CreateTexture("t3", Extent2D(3, 3), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST, 1, 2);// if use rgba8, need raw data also in u8 format when upload. todo
    auto t4 = device.CreateTexture("t4", Extent3D(4, 4, 4), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST);
    auto t5 = device.CreateTexture("t5", Extent2D(4, 4), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST, 1, 6);
    auto t6 = device.CreateTexture("t6", Extent2D(4, 4), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST, 1, 6 * 2);
    // not consider texture2dms
    TextureRef tex_arr[kNumTexArr];
    for (int i = 0; i < kNumTexArr; ++i) {
        tex_arr[i] = device.CreateTexture(std::format("tex_array_{}", i), Extent2D(5, 5), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST);
    }
    //auto rwt0 = device.CreateTexture("rwt0", Extent2D(2, 1), EPixelFormat::PF_R16G16B16A16_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);
    //auto rwt1 = device.CreateTexture("rwt1", Extent2D(2, 1), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST, 1, 2);
    auto rwt2 = device.CreateTexture("rwt2", Extent2D(3, 3), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);
    auto rwt3 = device.CreateTexture("rwt3", Extent2D(3, 3), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST, 1, 2);
    auto rwt4 = device.CreateTexture("rwt4", Extent3D(4, 4, 4), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);

    auto s0 = Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};

    auto bindless_array = device.CreateBindlessArray(1024);
    uint tex_handle     = bindless_array->AllocateTexture(t2->GetView(0, 3), s0);
    uint buf_handle     = bindless_array->AllocateBuffer(sb1->GetView());

    S0 pc{
        .a          = 2,
        .b          = 3,
        .tex_handle = tex_handle,
        .buf_handle = buf_handle,
    };
    S1 pc1{
        .a = 1,
    };

    auto   fence     = device.CreateFence();
    auto&  gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
    uint64 timeline  = 0;

    auto               pipeline = ShaderManager::Get().Compute<TestComputePipeline>("test/var.hlsl");
    std::vector<float> farr(1024), farr2(1024);
    std::vector<int>   iarr(1024);

    for (int iter = 0; iter < 9; ++iter) {

        for (int i = 0; i < 7; ++i) farr[i] = iarr[i] = 1 + i + iter;
        for (int i = 0; i < 7; ++i) farr2[i] = (i + 1) / 10.f;

        float res = pc.b * pc1.a;
        for (int i = 1; i < 7; ++i) res *= iarr[i];
        for (int i = 0; i < 5; ++i) res *= farr2[i];
        res *= farr2[1] * 10;
        res *= farr2[1] * 10;
        res *= farr2[2] * 10;
        res *= farr2[3] * 10;
        res *= farr2[0];
        res *= iarr[1];

        CommandList list;
        //list.CopyFrom(ToSpan(pc), cb0->GetView());
        list.CopyFrom(ToSpan(pc1), cb1->GetView());
        list.CopyFrom(ToSpan(farr).subspan(0 * sizeof(float), sizeof(float)), sb0->GetView());
        list.CopyFrom(ToSpan(iarr).subspan(1 * sizeof(float), sizeof(float)), sb1->GetView(1 * sizeof(float)));
        list.CopyFrom(ToSpan(iarr).subspan(2 * sizeof(float), sizeof(float)), rb0->GetView(2 * sizeof(float)));
        list.CopyFrom(ToSpan(farr).subspan(3 * sizeof(float), sizeof(float)), rb1->GetView(3 * sizeof(float)));
        list.CopyFrom(ToSpan(farr).subspan(4 * sizeof(float), sizeof(float)), tb0->GetView(4 * sizeof(float)));
        list.CopyFrom(ToSpan(farr).subspan(5 * sizeof(float), sizeof(float)), tb1->GetView(5 * sizeof(float)));
        list.CopyFrom(ToSpan(farr).subspan(6 * sizeof(float), sizeof(float)), buf_arr[6]->GetView(6 * sizeof(float)));

        auto view     = t2->GetView(1);
        view.extent.x = 1 << 1;// target extent 1, consider 1 mip
        view.offset.y = 1;
        view.extent.y = ((t2->GetHeight() >> 1) - 1) << 1;// -1 to compensate offset
        list.CopyFrom(ToSpan(farr2).subspan(0 * sizeof(float), sizeof(float)), view);
        list.CopyFrom(ToSpan(farr2).subspan(1 * sizeof(float), sizeof(float)), t3->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(2 * sizeof(float), sizeof(float)), t4->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(3 * sizeof(float), sizeof(float)), t5->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(4 * sizeof(float), sizeof(float)), t6->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(1 * sizeof(float), sizeof(float)), tex_arr[6]->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(1 * sizeof(float), sizeof(float)), rwt2->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(2 * sizeof(float), sizeof(float)), rwt3->GetView());
        list.CopyFrom(ToSpan(farr2).subspan(3 * sizeof(float), sizeof(float)), rwt4->GetView());

        Array<BufferView> buf_arr_view(kNumBufArr);
        for (int i = 0; i < kNumBufArr; ++i) {
            buf_arr_view[i] = buf_arr[i]->GetView(PF_R32_SFLOAT);
        }
        Array<TextureView> tex_arr_view(kNumTexArr);
        for (int i = 0; i < kNumTexArr; ++i) {
            tex_arr_view[i] = tex_arr[i]->GetView();
        }
        list.UpdateBindlessArray(bindless_array);
        list.Compute(pipeline, pc, cb1, sb0, sb1, rb0, rb1, tb0->GetView(PF_R32_SFLOAT), tb1->GetView(PF_R32_SFLOAT), std::span{buf_arr_view}, t2->GetView(0, 3), t3, t4, t5, t6, std::span{tex_arr_view}, rwt2, rwt3, rwt4, s0, bindless_array).Dispatch({1, 1, 1});
        //list.Compute(pipeline, cb0, cb1, sb0, sb1, rb0, rb1, tb0->GetView(PF_R32_SFLOAT), tb1->GetView(PF_R32_SFLOAT)).Dispatch({1, 1, 1});

        list.CopyFrom(rb1->GetView(0, 10 * sizeof(float)), ToSpan(iarr));

        list.CopyFrom(t2->GetView(1), tb0->GetView());
        list.CopyFrom(tb0->GetView(), ToSpan(farr2).subspan(1 * sizeof(float)));
        //list.CopyFrom(t2->GetView(1), ToSpan(farr2).subspan(1 * sizeof(float)));

        gfx_queue.Execute(list.Submit().Signal(fence, timeline));

        gfx_queue.Sync();
        LOG_INFO("dispatch work done");

        LOG_INFO("result={}, expect={}, ok={}", iarr[0], int(res), iarr[0] == int(res));
        for (int i = 0; i < 8; ++i) LOG_INFO("farr2[{}]: {}", i, farr2[i + 64]);// +64 to skip first row, see 't2' offset.y.  this skip one row(pitch), which is 256 bytes
        // expect [0, 0.1, 0.2, 0.3, 0.4, 0, 0, 0]
    }
}

void TestWindow() {
    using namespace Moer;
    using namespace Moer::Render;

    auto& device = RenderDevice::Get();

    uint2 resolution{1280, 720};
    WindowContext::Init(SurfaceInitInfo("D3D12", resolution.x, resolution.y, "MoerEditor", false));

    auto sc_info = SwapchainCreateInfo{
        .window_handle    = (uintptr_t)WindowContext::GetMainWindow(),
        .size             = resolution,
        .back_buffer_sz   = 3,
        .preferred_format = PF_R8G8B8A8_UNORM};

    auto sc = device.CreateSwapchain(sc_info);

    auto& gfx_queue = device.GetCommandQueue(EQueueType::Graphics);


    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        WindowContext::Tick();

        int w_width, w_height;
        WindowContext::GetWindowSize(WindowContext::GetMainWindow(), &w_width, &w_height);
        if (w_width == 0 || w_height == 0) {
            std::this_thread::yield();
            continue;
        }
        if (w_width != resolution.x || w_height != resolution.y) {
            resolution   = {uint32(w_width), uint32(w_height)};
            sc_info.size = {resolution.x, resolution.y};
            sc->Sync();
            sc->Recreate(sc_info);
            LOG_INFO("resize to {}x{}", resolution.x, resolution.y);
        }
        //std::this_thread::sleep_for(std::chrono::milliseconds(1));

        gfx_queue.Present(sc, {});// now only test clearrendertarget with color
    }
    gfx_queue.Sync();
}

int main(int argc, char** argv) {
    using namespace Moer;
    using namespace Moer::Render;

    std::filesystem::path path = argv[0];
    path.filename().string().find(".exe") != std::string::npos ? path = path.parent_path() : path = path;
    ConfigManager::GetInstance().Init(path);

    TaskSystem::Init();
    DeviceInitInfo info{
        .type = ERHIType::D3D12,
        .name = "DXRHITest",
        .rhi  = "d3d12"};
    //DeviceInitInfo info{
    //    .type = ERHIType::Vulkan,
    //    .name = "DXRHITest",
    //    .rhi  = "vulkan",
    //    .rhi_api_version = "1.3"};

    //auto capturer = CreatePIXCapturer();

    RenderDevice::Init(std::move(info));
    ShaderCompiler::Init();

    try {
        //capturer->Begin("D:\\codebase\\repos\\MoerEngine\\test.wpix");

        //TestBasic();
        TestWindow();

        //capturer->End();

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

    auto       packed_real_tex_and_sampler_idx_in_heap = g__array_114514_bdls[constants.tex_handle];
    Texture2D  tex              = gTexture2D_114514_bdls[packed_real_tex_and_sampler_idx_in_heap.tex];
    SamplerState sampler              = gTexture2D_114514_bdls[packed_real_tex_and_sampler_idx_in_heap.sampler];
    tex.Sample(sampler,...);

    */
    //ShaderCompilerEnvironment env{};

    ////ShaderCompilerInput       input{
    ////          .target_info               = ShaderTargetInfo(ST_COMPUTE, SP_WIN_D3D_SM6),
    ////          .mutation_id               = 0,
    ////          .entry_point               = "CSMain",
    ////          .relative_source_file_path = "test/FillArg.hlsl",
    ////          .shader_name               = "test/FillArg.hlsl",
    ////          .environment               = env};
    //ShaderCompilerInput input{
    //    .target_info               = ShaderTargetInfo(ST_COMPUTE, SP_WIN_D3D_SM6),
    //    .mutation_id               = 0,
    //    .entry_point               = "main",
    //    .relative_source_file_path = "test/var.hlsl",
    //    .shader_name               = "test/var.hlsl",
    //    .environment               = env};

    //ShaderCompiler::Init();
    ////auto output = ShaderCompiler::Compile(input);

    //auto pipeline = ShaderManager::Get().Compute<TemporalResmaplePipeline>("hwrt/ReSTIRDI/TemporalResampling.hlsl");

    return 0;
}
