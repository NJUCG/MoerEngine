#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <vector>

#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "render/rhi/RHIImpl.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

namespace {

using namespace Moer;
using namespace Moer::Render;

void ShutdownRHIForTest() {
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
}

constexpr uint32_t kElementCount      = 256;
constexpr uint32_t kIterations        = 64;
constexpr uint32_t kPresentIterations = 8;
constexpr uint32_t kCopyScopeIterations = 8;

struct IndicePair {
    uint32_t src;
    uint32_t dst;
};

template<typename T>
std::span<byte> ToByteSpan(std::vector<T>& values) {
    return std::span<byte>(reinterpret_cast<byte*>(values.data()), values.size() * sizeof(T));
}

void ApplyShuffle(
    const std::vector<uint32_t>& input,
    const std::vector<IndicePair>& pairs,
    std::vector<uint32_t>& output
) {
    std::fill(output.begin(), output.end(), 0u);
    for (const auto& pair : pairs) {
        output[pair.dst] = input[pair.src];
    }
}

std::vector<IndicePair> BuildPermutationPairs(uint32_t multiplier, uint32_t bias) {
    std::vector<IndicePair> pairs(kElementCount);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        pairs[i] = {.src = i, .dst = (i * multiplier + bias) % kElementCount};
    }
    return pairs;
}

void SubmitAndWait(Array<CommandList>&& command_lists) {
    RHIExecutor::Get().Submit(std::move(command_lists), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
}

bool ValidateResult(
    uint32_t iter,
    const std::vector<uint32_t>& expected,
    const std::vector<uint32_t>& got
) {
    if (expected == got) {
        return true;
    }
    uint32_t mismatch_count = 0;
    for (uint32_t i = 0; i < kElementCount; ++i) {
        if (expected[i] != got[i]) {
            LOG_ERROR(
                "Mismatch at iter={}, index={}, expected={}, got={}",
                iter,
                i,
                expected[i],
                got[i]
            );
            ++mismatch_count;
            if (mismatch_count >= 8) {
                break;
            }
        }
    }
    return false;
}

bool ValidateUniformValue(uint32_t iter, uint32_t expected, const std::vector<uint32_t>& got) {
    for (uint32_t i = 0; i < got.size(); ++i) {
        if (got[i] != expected) {
            LOG_ERROR(
                "Uniform mismatch at iter={}, index={}, expected={}, got={}",
                iter,
                i,
                expected,
                got[i]
            );
            return false;
        }
    }
    return true;
}

int RunCommandListQueueBindingTest() {
    CommandList graphics_cmd(EQueueType::Graphics);

    if (graphics_cmd.GetQueueType() != EQueueType::Graphics) {
        LOG_ERROR("Graphics command list queue binding mismatch");
        return 1;
    }

    LOG_INFO("CommandList queue binding test passed");
    return 0;
}

int RunRHITranslateMultiQueueReadbackTest() {
    auto& device = RenderDevice::Get();

    auto src = device.CreateBuffer<uint32_t>(
        "translate_src",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto mid = device.CreateBuffer<uint32_t>(
        "translate_mid",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC
    );
    auto dst = device.CreateBuffer<uint32_t>(
        "translate_dst",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC
    );
    auto indices_stage0 = device.CreateBuffer<IndicePair>(
        "translate_indices_stage0",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto indices_stage1 = device.CreateBuffer<IndicePair>(
        "translate_indices_stage1",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto shuffle_pipeline =
        ShaderManager::Get().Compute<ComponentShuffleShader>("core/utils/ShuffleBufferIndices.hlsl");

    auto perm0 = BuildPermutationPairs(37u, 11u);
    auto perm1 = BuildPermutationPairs(53u, 7u);

    std::vector<uint32_t> src_data(kElementCount);
    std::vector<uint32_t> stage0_expected(kElementCount);
    std::vector<uint32_t> final_expected(kElementCount);
    std::vector<uint32_t> readback_data(kElementCount);

    for (uint32_t iter = 0; iter < kIterations; ++iter) {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            src_data[i] = iter * 4096u + i * 3u + 17u;
        }

        ApplyShuffle(src_data, perm0, stage0_expected);
        ApplyShuffle(stage0_expected, perm1, final_expected);

        CommandList upload_cmd(EQueueType::Graphics);
        upload_cmd.CopyFrom(ToByteSpan(src_data), src->GetView());
        upload_cmd.CopyFrom(ToByteSpan(perm0), indices_stage0->GetView());
        upload_cmd.CopyFrom(ToByteSpan(perm1), indices_stage1->GetView());

        ComponentShuffleShader::Arg shuffle_args{
            .stride = 1u,
            .component_cnt = kElementCount,
        };

        CommandList compute_stage0_cmd(EQueueType::Graphics);
        compute_stage0_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage0->GetView(), src->GetView(), mid->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage0Dispatch");

        CommandList compute_stage1_cmd(EQueueType::Graphics);
        compute_stage1_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage1->GetView(), mid->GetView(), dst->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage1Dispatch");

        std::fill(readback_data.begin(), readback_data.end(), 0u);
        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event = readback_cmd.ReadbackCopy(dst->GetView(), ToByteSpan(readback_data));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(upload_cmd));
        frame_cmds.emplace_back(std::move(compute_stage0_cmd));
        frame_cmds.emplace_back(std::move(compute_stage1_cmd));
        frame_cmds.emplace_back(std::move(readback_cmd));

        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateResult(iter, final_expected, readback_data)) {
            return 1;
        }
    }

    LOG_INFO("RHI translate multiqueue readback test passed, iterations={}", kIterations);
    return 0;
}

int RunGraphicsCopyScopeRoundTripTest() {
    auto& device = RenderDevice::Get();
    auto scratch_buffer = device.CreateBuffer<uint32_t>(
        "copyscope_graphics_roundtrip_scratch",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto transfer_buffer = device.CreateBuffer<uint32_t>(
        "copyscope_graphics_roundtrip",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);

    for (uint32_t iter = 0; iter < kCopyScopeIterations; ++iter) {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            upload_values[i] = iter * 2048u + i * 5u + 9u;
        }
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList graphics_cmd(EQueueType::Graphics);
        graphics_cmd.ClearResource(scratch_buffer->GetView(), 0u);
        {
            auto copy_scope = graphics_cmd.BeginCopyScope();
            copy_scope.CopyFrom(ToByteSpan(upload_values), transfer_buffer->GetView());
        }
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(transfer_buffer->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));
        SubmitAndWait(std::move(frame_cmds));
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateResult(iter, upload_values, readback_values)) {
            return 1;
        }
    }

    LOG_INFO("Graphics -> CopyScope -> Graphics test passed, iterations={}", kCopyScopeIterations);
    return 0;
}

int RunMultiCopyScopeOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer_a = device.CreateBuffer<uint32_t>(
        "copyscope_multi_scope_a",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );
    auto buffer_b = device.CreateBuffer<uint32_t>(
        "copyscope_multi_scope_b",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::UNORDERED_ACCESS
    );

    std::vector<uint32_t> values_a(kElementCount);
    std::vector<uint32_t> values_b(kElementCount);
    std::vector<uint32_t> readback_a(kElementCount, 0u);
    std::vector<uint32_t> readback_b(kElementCount, 0u);

    for (uint32_t i = 0; i < kElementCount; ++i) {
        values_a[i] = i * 7u + 1u;
        values_b[i] = i * 11u + 5u;
    }

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(values_a), buffer_a->GetView());
    }
    GraphEventRef readback_a_event = graphics_cmd.ReadbackCopy(buffer_a->GetView(), ToByteSpan(readback_a));
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(values_b), buffer_b->GetView());
    }
    GraphEventRef readback_b_event = graphics_cmd.ReadbackCopy(buffer_b->GetView(), ToByteSpan(readback_b));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    SubmitAndWait(std::move(frame_cmds));
    if (readback_a_event) {
        readback_a_event->Wait();
    }
    if (readback_b_event) {
        readback_b_event->Wait();
    }

    if (!ValidateResult(0u, values_a, readback_a)) {
        return 1;
    }
    if (!ValidateResult(1u, values_b, readback_b)) {
        return 1;
    }

    LOG_INFO("Multi-CopyScope ordering test passed");
    return 0;
}

int RunCopyScopeUnknownFirstUseTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "copyscope_unknown_first_use",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0xABC000u + i;
    }

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        auto copy_scope = graphics_cmd.BeginCopyScope();
        copy_scope.CopyFrom(ToByteSpan(upload_values), buffer->GetView());
    }
    GraphEventRef readback_event = graphics_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    SubmitAndWait(std::move(frame_cmds));
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO("CopyScope unknown-first-use test passed");
    return 0;
}

int RunPresentWithCopyScopeTests() {
    auto& device = RenderDevice::Get();
    auto* window = WindowContext::GetMainWindow();
    if (window == nullptr) {
        LOG_ERROR("CopyScope present test window is null.");
        return 1;
    }

    constexpr uint32_t kWidth  = 640;
    constexpr uint32_t kHeight = 360;

    SwapchainCreateInfo swapchain_ci{
        .window_handle = reinterpret_cast<uintptr_t>(window),
        .size = {kWidth, kHeight},
        .back_buffer_sz = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    SwapchainRef swapchain = device.CreateSwapchain(swapchain_ci);
    TextureRef   output    = device.CreateTexture(
        "translate_present_copyscope_output",
        Extent2D(kWidth, kHeight),
        swapchain->format,
        ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST
    );

    std::vector<uint32_t> upload_values(kWidth * kHeight, 0u);
    std::vector<uint32_t> readback_values(kWidth * kHeight, 0u);

    for (uint32_t iter = 0; iter < kPresentIterations; ++iter) {
        WindowContext::Tick();

        std::fill(readback_values.begin(), readback_values.end(), 0u);
        std::fill(upload_values.begin(), upload_values.end(), 0xFF000000u | (iter * 131u + 23u));

        CommandList graphics_cmd(EQueueType::Graphics);
        {
            auto copy_scope = graphics_cmd.BeginCopyScope();
            copy_scope.CopyFrom(ToByteSpan(upload_values), output->GetView());
        }
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
            &present_request
        );
        if (readback_event) {
            readback_event->Wait();
        }
        RHIExecutor::Get().Sync(ERHISyncDepth::Present);

        if (upload_values != readback_values) {
            LOG_ERROR("CopyScope present readback mismatch at iter={}", iter);
            return 1;
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    LOG_INFO("Present + CopyScope test passed, iterations={}", kPresentIterations);
    return 0;
}

int RunPresentTests() {
    auto& device = RenderDevice::Get();
    auto* window = WindowContext::GetMainWindow();
    if (window == nullptr) {
        LOG_ERROR("Present test window is null.");
        return 1;
    }

    constexpr uint32_t kWidth  = 640;
    constexpr uint32_t kHeight = 360;

    SwapchainCreateInfo swapchain_ci{
        .window_handle = reinterpret_cast<uintptr_t>(window),
        .size = {kWidth, kHeight},
        .back_buffer_sz = 2,
        .preferred_format = PF_R8G8B8A8_SRGB
    };
    SwapchainRef swapchain = device.CreateSwapchain(swapchain_ci);
    TextureRef   output    = device.CreateTexture(
        "translate_present_output",
        Extent2D(kWidth, kHeight),
        swapchain->format,
        ETextureUsageFlags::TRANSFER_SRC | ETextureUsageFlags::TRANSFER_DST
    );

    auto graphics_buffer = device.CreateBuffer<uint32_t>(
        "translate_present_graphics_buf",
        kElementCount,
        EBufferUsageFlags::TRANSFER_SRC | EBufferUsageFlags::TRANSFER_DST
    );
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    std::vector<uint32_t> output_values(kWidth * kHeight, 0u);

    for (uint32_t iter = 0; iter < kPresentIterations; ++iter) {
        WindowContext::Tick();

        for (uint32_t i = 0; i < kElementCount; ++i) {
            readback_values[i] = 0u;
        }
        std::fill(output_values.begin(), output_values.end(), 0xFF000000u | (iter * 97u + 17u));

        CommandList graphics_cmd(EQueueType::Graphics);
        graphics_cmd.ClearResource(graphics_buffer->GetView(), iter + 1u);
        GraphEventRef readback_event =
            graphics_cmd.ReadbackCopy(graphics_buffer->GetView(), ToByteSpan(readback_values));
        graphics_cmd.CopyFrom(ToByteSpan(output_values), output->GetView());

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
            &present_request
        );
        if (readback_event) {
            readback_event->Wait();
        }

        if (!ValidateUniformValue(iter, iter + 1u, readback_values)) {
            return 1;
        }
    }

    RHIExecutor::Get().Sync(ERHISyncDepth::Present);
    LOG_INFO("RHI translate present tests passed, iterations={}", kPresentIterations);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path path = (argc > 0) ? std::filesystem::path(argv[0]) : std::filesystem::current_path();
    if (path.extension() == ".exe") {
        path = path.parent_path();
    }

    Moer::ConfigManager::GetInstance().Init(path);
    Moer::LogSystem::Init();
    Moer::TaskSystem::Init();

    DeviceInitInfo info{
        .rhi_type = ERHIType::Vulkan,
        .name = "TestRHITranslate",
        .rhi_api_version = "1.3",
    };

    if (info.rhi_type != ERHIType::Vulkan) {
        std::cout << "SKIP (Vulkan-only)" << std::endl;
        return 0;
    }

    bool window_inited = false;
    try {
        RenderDevice::Init(std::move(info));
        ShaderCompiler::Init();

        const int queue_ret = RunCommandListQueueBindingTest();
        if (queue_ret != 0) {
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return queue_ret;
        }

        const int translate_readback_ret = RunRHITranslateMultiQueueReadbackTest();
        if (translate_readback_ret != 0) {
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return translate_readback_ret;
        }

        const int graphics_copyscope_ret = RunGraphicsCopyScopeRoundTripTest();
        if (graphics_copyscope_ret != 0) {
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return graphics_copyscope_ret;
        }

        const int multi_scope_ret = RunMultiCopyScopeOrderingTest();
        if (multi_scope_ret != 0) {
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return multi_scope_ret;
        }

        const int unknown_first_use_ret = RunCopyScopeUnknownFirstUseTest();
        if (unknown_first_use_ret != 0) {
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return unknown_first_use_ret;
        }

        WindowContext::Init(SurfaceInitInfo(ERHIType::Vulkan, 640, 360, "TestRHITranslatePresent", false));
        window_inited = true;
        const int present_copyscope_ret = RunPresentWithCopyScopeTests();
        if (present_copyscope_ret != 0) {
            if (window_inited) {
                WindowContext::ShutDown();
                window_inited = false;
            }
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return present_copyscope_ret;
        }
        const int present_ret = RunPresentTests();
        if (present_ret != 0) {
            if (window_inited) {
                WindowContext::ShutDown();
                window_inited = false;
            }
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return present_ret;
        }
        if (window_inited) {
            WindowContext::ShutDown();
            window_inited = false;
        }
        ShaderManager::ShutDown();
        ShutdownRHIForTest();
        Moer::TaskSystem::ShutDown();
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("TestRHITranslate failed: {}", e.what());
        if (window_inited) {
            WindowContext::ShutDown();
        }
        ShaderManager::ShutDown();
        ShutdownRHIForTest();
        Moer::TaskSystem::ShutDown();
        return 1;
    }
}
