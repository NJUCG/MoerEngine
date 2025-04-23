
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
        float2 _pad;
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

        DEFINE_SHADER_ARGS(cb0, cb1, sb0, sb1, rb0, rb1, tb0, tb1, buf_arr, t2, t3, t4, t5, t6, tex_arr, rwt2, rwt3, rwt4, s0);
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

    auto capturer = CreatePIXCapturer();

    RenderDevice::Init(std::move(info));

    try {
        capturer->Begin("D:\\codebase\\repos\\MoerEngine\\test.wpix");

        auto& device = RenderDevice::Get();

        ShaderCompiler::Init();

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
        auto t2 = device.CreateTexture("t2", Extent2D(3, 3), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::TRANSFER_DST);
        auto t3 = device.CreateTexture("t3", Extent2D(3, 3), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST, 1, 2);
        auto t4 = device.CreateTexture("t4", Extent3D(4, 4, 4), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST);
        auto t5 = device.CreateTexture("t5", Extent2D(4, 4), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST, 1, 6);
        auto t6 = device.CreateTexture("t6", Extent2D(4, 4), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST, 1, 6 * 2);
        // not consider texture2dms
        TextureRef tex_arr[kNumTexArr];
        for (int i = 0; i < kNumTexArr; ++i) {
            tex_arr[i] = device.CreateTexture(std::format("tex_array_{}", i), Extent2D(5, 5), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::TRANSFER_DST);
        }
        //auto rwt0 = device.CreateTexture("rwt0", Extent2D(2, 1), EPixelFormat::PF_R16G16B16A16_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);
        //auto rwt1 = device.CreateTexture("rwt1", Extent2D(2, 1), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST, 1, 2);
        auto rwt2 = device.CreateTexture("rwt2", Extent2D(3, 3), EPixelFormat::PF_R32G32B32A32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);
        auto rwt3 = device.CreateTexture("rwt3", Extent2D(3, 3), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST, 1, 2);
        auto rwt4 = device.CreateTexture("rwt4", Extent3D(4, 4, 4), EPixelFormat::PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::TRANSFER_DST);

        // todo test copy buffer to texture

        auto s0 = Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};

        S0 pc{
            .a = 2,
            .b = 3,
        };
        S1 pc1{
            .a = 1,
        };

        auto   fence     = device.CreateFence();
        auto&  gfx_queue = device.GetCommandQueue(EQueueType::Graphics);
        uint64 timeline  = 0;

        auto               pipeline = ShaderManager::Get().Compute<TestComputePipeline>("test/var.hlsl");
        std::vector<float> farr(1024);
        std::vector<int>   iarr(1024);

        for (int iter = 0; iter < 10; ++iter) {

            for (int i = 0; i < 7; ++i) farr[i] = iarr[i] = 1 + i + iter;

            int res = pc.b * pc1.a;
            for (int i = 1; i < 7; ++i) res *= iarr[i];

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

            Array<BufferView> buf_arr_view(kNumBufArr);
            for (int i = 0; i < kNumBufArr; ++i) {
                buf_arr_view[i] = buf_arr[i]->GetView(PF_R32_SFLOAT);
            }
            Array<TextureView> tex_arr_view(kNumTexArr);
            for (int i = 0; i < kNumTexArr; ++i) {
                tex_arr_view[i] = tex_arr[i]->GetView();
            }
            list.Compute(pipeline, pc, cb1, sb0, sb1, rb0, rb1, tb0->GetView(PF_R32_SFLOAT), tb1->GetView(PF_R32_SFLOAT), std::span{buf_arr_view},
                t2, t3, t4, t5, t6, std::span{tex_arr_view}, rwt2, rwt3, rwt4, s0).Dispatch({1, 1, 1});
            //list.Compute(pipeline, cb0, cb1, sb0, sb1, rb0, rb1, tb0->GetView(PF_R32_SFLOAT), tb1->GetView(PF_R32_SFLOAT)).Dispatch({1, 1, 1});

            list.CopyFrom(rb1->GetView(0, 10 * sizeof(float)), ToSpan(iarr));

            gfx_queue.Execute(list.Submit().Signal(fence, timeline));

            gfx_queue.Sync();
            LOG_INFO("dispatch work done");

            LOG_INFO("result={}, expect={}, ok={}", iarr[0], res, iarr[0] == res);
        }

        capturer->End();

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
