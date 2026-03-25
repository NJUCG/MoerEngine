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

constexpr uint32_t kElementCount      = 256;
constexpr uint32_t kIterations        = 64;
constexpr uint32_t kPresentIterations = 8;

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

bool ValidateResult(
    uint32_t iter,
    const std::vector<uint32_t>& expected,
    const std::vector<uint32_t>& got
) {
    if (expected == got) {
        return true;
    }
    for (uint32_t i = 0; i < kElementCount; ++i) {
        if (expected[i] != got[i]) {
            LOG_ERROR(
                "Mismatch at iter={}, index={}, expected={}, got={}",
                iter,
                i,
                expected[i],
                got[i]
            );
            break;
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
    CommandList compute_cmd(EQueueType::Compute);

    if (graphics_cmd.GetQueueType() != EQueueType::Graphics) {
        LOG_ERROR("Graphics command list queue binding mismatch");
        return 1;
    }
    if (compute_cmd.GetQueueType() != EQueueType::Compute) {
        LOG_ERROR("Compute command list queue binding mismatch");
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

        CommandList compute_stage0_cmd(EQueueType::Compute);
        compute_stage0_cmd.Barriers(
            EQueueType::Graphics,
            EQueueType::Compute,
            EPassType::Compute,
            ReadBuffer{indices_stage0->GetView(), EBufferState::SHADER_RESOURCE},
            ReadBuffer{src->GetView(), EBufferState::SHADER_RESOURCE},
            WriteBuffer{mid->GetView(), EBufferState::UNORDERED_ACCESS}
        );
        compute_stage0_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage0->GetView(), src->GetView(), mid->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage0Dispatch");

        CommandList compute_stage1_cmd(EQueueType::Compute);
        compute_stage1_cmd.Barriers(
            EQueueType::Compute,
            EQueueType::Compute,
            EPassType::Compute,
            ReadBuffer{indices_stage1->GetView(), EBufferState::SHADER_RESOURCE},
            ReadBuffer{mid->GetView(), EBufferState::SHADER_RESOURCE},
            WriteBuffer{dst->GetView(), EBufferState::UNORDERED_ACCESS}
        );
        compute_stage1_cmd
            .Compute(shuffle_pipeline, shuffle_args, indices_stage1->GetView(), mid->GetView(), dst->GetView())
            .Dispatch((kElementCount + 63u) / 64u, "TranslateStage1Dispatch");

        std::fill(readback_data.begin(), readback_data.end(), 0u);
        CommandList readback_cmd(EQueueType::Graphics);
        readback_cmd.BufferBarriers(
            EQueueType::Compute,
            EQueueType::Graphics,
            EPassType::Copy,
            Array<ReadBuffer>{ReadBuffer{dst->GetView(), EBufferState::TRANSFER}},
            Array<WriteBuffer>{}
        );
        readback_cmd.CopyFrom(dst->GetView(), ToByteSpan(readback_data));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(upload_cmd));
        frame_cmds.emplace_back(std::move(compute_stage0_cmd));
        frame_cmds.emplace_back(std::move(compute_stage1_cmd));
        frame_cmds.emplace_back(std::move(readback_cmd));

        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        device.WaitIdle();

        if (!ValidateResult(iter, final_expected, readback_data)) {
            return 1;
        }
    }

    LOG_INFO("RHI translate multiqueue readback test passed, iterations={}", kIterations);
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

    auto compute_buffer = device.CreateBuffer<uint32_t>(
        "translate_present_compute_buf",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC |
            EBufferUsageFlags::TRANSFER_DST
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

        CommandList compute_cmd(EQueueType::Compute);
        compute_cmd.ClearResource(compute_buffer->GetView(), iter + 1u);

        CommandList graphics_cmd(EQueueType::Graphics);
        graphics_cmd.BufferBarriers(
            EQueueType::Compute,
            EQueueType::Graphics,
            EPassType::Copy,
            Array<ReadBuffer>{ReadBuffer{compute_buffer->GetView(), EBufferState::TRANSFER}},
            Array<WriteBuffer>{WriteBuffer{graphics_buffer->GetView(), EBufferState::TRANSFER}}
        );
        graphics_cmd.CopyFrom(compute_buffer->GetView(), graphics_buffer->GetView(), "ComputeToGraphics");
        graphics_cmd.BufferBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Copy,
            Array<ReadBuffer>{ReadBuffer{graphics_buffer->GetView(), EBufferState::TRANSFER}},
            Array<WriteBuffer>{}
        );
        graphics_cmd.CopyFrom(graphics_buffer->GetView(), ToByteSpan(readback_values));
        graphics_cmd.CopyFrom(ToByteSpan(output_values), output->GetView());

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(compute_cmd));
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
            &present_request
        );
        device.WaitIdle();

        if (!ValidateUniformValue(iter, iter + 1u, readback_values)) {
            return 1;
        }
    }

    swapchain->Sync();
    device.WaitIdle();
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
            RenderDevice::Dispose();
            Moer::TaskSystem::ShutDown();
            return queue_ret;
        }

        const int readback_ret = RunRHITranslateMultiQueueReadbackTest();
        if (readback_ret != 0) {
            ShaderManager::ShutDown();
            RenderDevice::Dispose();
            Moer::TaskSystem::ShutDown();
            return readback_ret;
        }

        WindowContext::Init(SurfaceInitInfo(ERHIType::Vulkan, 640, 360, "TestRHITranslatePresent", false));
        window_inited = true;
        const int present_ret = RunPresentTests();

        if (window_inited) {
            WindowContext::ShutDown();
            window_inited = false;
        }
        ShaderManager::ShutDown();
        RenderDevice::Dispose();
        Moer::TaskSystem::ShutDown();
        return present_ret;
    } catch (const std::exception& e) {
        LOG_ERROR("TestRHITranslate failed: {}", e.what());
        if (window_inited) {
            WindowContext::ShutDown();
        }
        ShaderManager::ShutDown();
        RenderDevice::Dispose();
        Moer::TaskSystem::ShutDown();
        return 1;
    }
}
