#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "render/rhi/RHIImpl.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/GPUEventStream.h"
#include "rhi/vulkan/VulkanDescriptor.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

namespace {

using namespace Moer;
using namespace Moer::Render;

struct BindlessReadbackArgs {
    uint32_t src_handle;
    uint32_t xor_mask;
    uint32_t element_count;
};

struct BindlessTextureReadbackArgs {
    uint32_t handle0;
    uint32_t handle1;
    uint32_t output_offset;
    uint32_t sample_count;
    float    uv0_x;
    float    uv0_y;
    float    uv1_x;
    float    uv1_y;
    float    mip0;
    float    mip1;
};

class BindlessBufferReadbackPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessBufferReadbackPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(BindlessReadbackArgs, args);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(args, output_buffer, bdls);
};

class BindlessTextureReadbackPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(BindlessTextureReadbackPipeline);

    DEFINE_SHADER_CONSTANT_STRUCT(BindlessTextureReadbackArgs, args);
    DEFINE_SHADER_BUFFER(output_buffer);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);

    DEFINE_SHADER_ARGS(args, output_buffer, bdls);
};

void ShutdownRHIForTest() {
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
}

constexpr uint32_t kElementCount      = 256;
constexpr uint32_t kIterations        = 64;
constexpr uint32_t kPresentIterations = 8;
constexpr uint32_t kCopyScopeIterations = 8;

template<typename Fn>
int RunNamedTestCase(const char* name, Fn&& fn) {
    LOG_INFO("[TESTCASE][BEGIN] {}", name);
    const int ret = fn();
    if (ret == 0) {
        LOG_INFO("[TESTCASE][PASS] {}", name);
    } else {
        LOG_ERROR("[TESTCASE][FAIL] {} :: exit={}", name, ret);
    }
    return ret;
}

struct IndicePair {
    uint32_t src;
    uint32_t dst;
};

template<typename T>
std::span<Moer::byte> ToByteSpan(std::vector<T>& values) {
    return std::span<Moer::byte>(reinterpret_cast<Moer::byte*>(values.data()), values.size() * sizeof(T));
}

std::vector<uint8_t> MakeSolidRgba8(uint32_t width, uint32_t height, uint8_t red) {
    std::vector<uint8_t> bytes(size_t(width) * size_t(height) * 4u, 0u);
    for (size_t i = 0; i < size_t(width) * size_t(height); ++i) {
        bytes[i * 4u + 0u] = red;
        bytes[i * 4u + 1u] = 0u;
        bytes[i * 4u + 2u] = 0u;
        bytes[i * 4u + 3u] = 255u;
    }
    return bytes;
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

bool ValidateAllocatedRanges(const std::vector<uint64_t>& offsets, uint64_t alloc_size) {
    if (offsets.empty()) {
        LOG_ERROR("Concurrent descriptor range test produced no offsets");
        return false;
    }

    std::vector<uint64_t> sorted = offsets;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] < sorted[i - 1] + alloc_size) {
            LOG_ERROR(
                "Descriptor range overlap detected: prev={}, current={}, alloc_size={}",
                sorted[i - 1],
                sorted[i],
                alloc_size
            );
            return false;
        }
    }
    return true;
}

int RunDescriptorHeapConcurrentRangeAllocationTest() {
    LOG_INFO("Descriptor heap concurrent range allocation test started");
    auto* vk_device = dynamic_cast<VulkanDevice*>(RenderDevice::Get().GetImpl());
    if (vk_device == nullptr) {
        LOG_ERROR("Descriptor heap range allocation test requires VulkanDevice");
        return 1;
    }

    VulkanDescriptorHeap& descriptor_heap = vk_device->GetGlobalDescriptorHeap();
    VulkanDescriptorBinder binder = descriptor_heap.BeginPushDescriptors();
    if (!binder.IsValid()) {
        LOG_ERROR("Descriptor heap failed to start concurrent range-allocation binder");
        return 1;
    }

    constexpr uint32_t kThreadCount = 8;
    constexpr uint32_t kAllocationsPerThread = 24;
    const uint64_t alignment =
        vk_device->GetOptionalProperties().descriptor_buffer_properties.descriptorBufferOffsetAlignment;
    const uint64_t alloc_size = Moer::AlignUp(uint64_t(64), alignment);

    std::vector<uint64_t> allocated_offsets;
    allocated_offsets.reserve(kThreadCount * kAllocationsPerThread);
    std::mutex allocation_mutex;
    std::array<std::thread, kThreadCount> threads;
    for (uint32_t thread_idx = 0; thread_idx < kThreadCount; ++thread_idx) {
        threads[thread_idx] = std::thread([&]() {
            binder.ActivateOnCurrentThread();
            std::vector<uint64_t> local_offsets;
            local_offsets.reserve(kAllocationsPerThread);
            for (uint32_t i = 0; i < kAllocationsPerThread; ++i) {
                local_offsets.push_back(descriptor_heap.AllocateOnlineDescriptorRange(alloc_size));
            }
            binder.DeactivateOnCurrentThread();
            std::lock_guard<std::mutex> lock(allocation_mutex);
            allocated_offsets.insert(
                allocated_offsets.end(), local_offsets.begin(), local_offsets.end()
            );
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    LOG_INFO("Descriptor heap test: concurrent range allocation finished");

    if (!ValidateAllocatedRanges(allocated_offsets, alloc_size)) {
        binder = descriptor_heap.EndPushDescriptors(std::move(binder));
        descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
        return 1;
    }

    const uint64_t trailing_offset = descriptor_heap.AllocateOnlineDescriptorRange(alloc_size);
    const uint64_t max_offset = *std::max_element(allocated_offsets.begin(), allocated_offsets.end());
    if (trailing_offset < max_offset + alloc_size) {
        LOG_ERROR(
            "Trailing descriptor allocation overlapped earlier concurrent allocations: trailing={}, max={}",
            trailing_offset,
            max_offset
        );
        binder = descriptor_heap.EndPushDescriptors(std::move(binder));
        descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
        return 1;
    }

    binder = descriptor_heap.EndPushDescriptors(std::move(binder));
    descriptor_heap.RecycleOnlineDescriptorLease(std::move(binder));
    LOG_INFO("Descriptor heap concurrent range allocation test passed");
    return 0;
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

int RunTranslateExecutionClassRoundTripTest() {
    const RHITranslateFence translate_fence = RHITranslateFence::Create();

    CommandList cmd(EQueueType::Graphics);
    cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    cmd.LambdaCommand([] {});
    cmd.TranslateFence(translate_fence);

    CmdSubmit submit = cmd.Submit();
    if (submit.translate_execution_class != ERHITranslateExecutionClass::SerialControl) {
        LOG_ERROR("Translate execution class did not round-trip through CommandList::Submit");
        return 1;
    }
    if (submit.cmds.size() != 2 || submit.cmds.back()->Type() != Command::EType::Custom) {
        LOG_ERROR("Translate fence command did not round-trip through CommandList::Submit");
        return 1;
    }

    const auto* custom_cmd = static_cast<const CustomCmd*>(submit.cmds.back().get());
    if (custom_cmd->CustomId() != CustomCmd::CustomCmdId::CUSTOM_TRANSLATE_FENCE) {
        LOG_ERROR("Submitted command was not a TranslateFence command");
        return 1;
    }

    const auto* translate_fence_cmd = static_cast<const TranslateFenceCmd*>(custom_cmd);
    if (translate_fence_cmd->Fence().event.Get() != translate_fence.event.Get()) {
        LOG_ERROR("Translate fence event did not round-trip through CommandList::Submit");
        return 1;
    }

    translate_fence.event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    LOG_INFO("Translate execution control round-trip test passed");
    return 0;
}

int RunTranslateLambdaCommandTest() {
    std::atomic_uint32_t lambda_counter{0};

    CommandList cmd(EQueueType::Graphics);
    cmd.LambdaCommand([&lambda_counter]() {
        lambda_counter.fetch_add(1, std::memory_order_relaxed);
    });

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(cmd));
    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (lambda_counter.load(std::memory_order_relaxed) != 1u) {
        LOG_ERROR("LambdaCommand did not execute exactly once during translate");
        return 1;
    }

    LOG_INFO("Translate LambdaCommand test passed");
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

int RunMultiCommandListSubmitOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "translate_multicmd_submit_order",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0x120000u + i * 13u + 7u;
    }

    CommandList upload_cmd(EQueueType::Graphics);
    upload_cmd.CopyFrom(ToByteSpan(upload_values), buffer->GetView());

    CommandList readback_cmd(EQueueType::Graphics);
    GraphEventRef readback_event =
        readback_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(upload_cmd));
    frame_cmds.emplace_back(std::move(readback_cmd));

    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO("Multi-commandlist submit ordering test passed");
    return 0;
}

int RunSerialControlTranslateOrderingTest() {
    auto& device = RenderDevice::Get();
    auto buffer = device.CreateBuffer<uint32_t>(
        "translate_serial_control_order",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);
    for (uint32_t i = 0; i < kElementCount; ++i) {
        upload_values[i] = 0x560000u + i * 17u + 3u;
    }

    CommandList upload_cmd(EQueueType::Graphics);
    upload_cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    upload_cmd.CopyFrom(ToByteSpan(upload_values), buffer->GetView());

    CommandList readback_cmd(EQueueType::Graphics);
    readback_cmd.SetTranslateExecutionClass(ERHITranslateExecutionClass::SerialControl);
    GraphEventRef readback_event =
        readback_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(upload_cmd));
    frame_cmds.emplace_back(std::move(readback_cmd));

    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }

    if (!ValidateResult(0u, upload_values, readback_values)) {
        return 1;
    }

    LOG_INFO("SerialControl translate ordering test passed");
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

int RunBindlessBufferReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array = device.CreateBindlessArray();
    auto src_a = device.CreateBuffer<uint32_t>(
        "bindless_src_a",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto src_b = device.CreateBuffer<uint32_t>(
        "bindless_src_b",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );
    auto output = device.CreateBuffer<uint32_t>(
        "bindless_readback_output",
        kElementCount,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessBufferReadbackPipeline>(
        "tests/BindlessReadback.comp.hlsl"
    );

    std::vector<uint32_t> upload_values(kElementCount);
    std::vector<uint32_t> expected_values(kElementCount);
    std::vector<uint32_t> readback_values(kElementCount, 0u);

    const auto run_case = [&](uint32_t iter,
                              const BufferRef& src,
                              uint32_t src_handle,
                              uint32_t base,
                              uint32_t stride,
                              uint32_t bias,
                              uint32_t xor_mask) -> bool {
        for (uint32_t i = 0; i < kElementCount; ++i) {
            upload_values[i] = base + i * stride + bias;
            expected_values[i] = upload_values[i] ^ xor_mask;
        }
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(upload_values), src->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessReadbackArgs args{
            .src_handle = src_handle,
            .xor_mask = xor_mask,
            .element_count = kElementCount,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch((kElementCount + 63u) / 64u, "BindlessBufferReadbackDispatch");

        GraphEventRef readback_event =
            cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        return ValidateResult(iter, expected_values, readback_values);
    };

    const uint32_t src_handle_a = bindless_array->AllocateBuffer(src_a->GetView());
    if (!run_case(0u, src_a, src_handle_a, 0x1000u, 3u, 7u, 0x13572468u)) {
        return 1;
    }

    const uint32_t src_handle_b = bindless_array->AllocateBuffer(src_b->GetView());
    if (!run_case(1u, src_b, src_handle_b, 0x4000u, 5u, 11u, 0x89ABCDEFu)) {
        return 1;
    }

    LOG_INFO(
        "Bindless buffer readback test passed, handles=({}, {})",
        src_handle_a,
        src_handle_b
    );
    return 0;
}

int RunBindlessTextureReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array = device.CreateBindlessArray();
    auto output = device.CreateBuffer<uint32_t>(
        "bindless_texture_readback_output",
        8u,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessTextureReadbackPipeline>(
        "tests/BindlessTextureReadback.comp.hlsl"
    );

    std::vector<uint32_t> readback_values(8u, 0u);

    const auto readback_and_validate = [&](const std::vector<uint32_t>& expected,
                                           const char*                 label) -> bool {
        std::fill(readback_values.begin(), readback_values.end(), 0u);

        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event =
            readback_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(readback_cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }

        for (size_t i = 0; i < expected.size(); ++i) {
            if (readback_values[i] != expected[i]) {
                LOG_ERROR(
                    "{} mismatch at index={}, expected={}, got={}",
                    label,
                    i,
                    expected[i],
                    readback_values[i]
                );
                return false;
            }
        }
        return true;
    };

    auto mip_texture = device.CreateTexture(
        "bindless_texture_mip",
        Extent2D(2u, 2u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED,
        2u
    );
    auto mip0_data = MakeSolidRgba8(2u, 2u, 64u);
    auto mip1_data = MakeSolidRgba8(1u, 1u, 192u);
    const uint32_t mip_handle = bindless_array->AllocateTexture(
        mip_texture->GetView(0u, 2u),
        Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE}
    );

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(mip0_data), mip_texture->GetView(0u));
        cmd.CopyFrom(ToByteSpan(mip1_data), mip_texture->GetView(1u));
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs args{
            .handle0 = mip_handle,
            .handle1 = mip_handle,
            .output_offset = 0u,
            .sample_count = 2u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 1.0f,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureMipDispatch");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({64u, 192u}, "BindlessTextureMip")) {
        return 1;
    }

    auto sampler_texture = device.CreateTexture(
        "bindless_texture_sampler",
        Extent2D(2u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    std::vector<uint8_t> sampler_data{
        0u, 0u, 0u, 255u,
        255u, 0u, 0u, 255u,
    };
    const uint32_t sampler_handle_clamp = bindless_array->AllocateTexture(
        sampler_texture->GetView(),
        Sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE}
    );
    const uint32_t sampler_handle_repeat = bindless_array->AllocateTexture(
        sampler_texture->GetView(),
        Sampler{SF_NEAREST, SAM_REPEAT}
    );

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(sampler_data), sampler_texture->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs args{
            .handle0 = sampler_handle_clamp,
            .handle1 = sampler_handle_repeat,
            .output_offset = 0u,
            .sample_count = 2u,
            .uv0_x = 1.25f,
            .uv0_y = 0.5f,
            .uv1_x = 1.25f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };

        cmd.Compute(pipeline, args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureSamplerDispatch");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({255u, 0u}, "BindlessTextureSampler")) {
        return 1;
    }

    auto update_texture_a = device.CreateTexture(
        "bindless_texture_update_a",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto update_texture_b = device.CreateTexture(
        "bindless_texture_update_b",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto update_data_a = MakeSolidRgba8(1u, 1u, 32u);
    auto update_data_b = MakeSolidRgba8(1u, 1u, 224u);
    const Sampler update_sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};
    const uint32_t update_handle =
        bindless_array->AllocateTexture(update_texture_a->GetView(), update_sampler);
    uint32_t rebound_handle = 0u;

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(update_data_a), update_texture_a->GetView());
        cmd.CopyFrom(ToByteSpan(update_data_b), update_texture_b->GetView());
        cmd.ClearResource(output->GetView(), 0u);
        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs dispatch_a_args{
            .handle0 = update_handle,
            .handle1 = update_handle,
            .output_offset = 0u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        cmd.Compute(pipeline, dispatch_a_args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureUpdateDispatchA");

        rebound_handle = bindless_array->AllocateTexture(update_texture_b->GetView(), update_sampler);

        cmd.UpdateBindlessArray(bindless_array);

        BindlessTextureReadbackArgs dispatch_b_args{
            .handle0 = rebound_handle,
            .handle1 = rebound_handle,
            .output_offset = 1u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        cmd.Compute(pipeline, dispatch_b_args, output->GetView(), bindless_array)
            .Dispatch(1u, "BindlessTextureUpdateDispatchB");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    if (!readback_and_validate({32u, 224u}, "BindlessTextureUpdate")) {
        return 1;
    }

    LOG_INFO(
        "Bindless texture readback test passed, mip_handle={}, sampler_handles=({}, {}), update_handle={}",
        mip_handle,
        sampler_handle_clamp,
        sampler_handle_repeat,
        rebound_handle
    );
    return 0;
}

int RunMultiBindlessArrayReadbackTest() {
    auto& device = RenderDevice::Get();

    auto bindless_array_a = device.CreateBindlessArray();
    auto bindless_array_b = device.CreateBindlessArray();
    auto output = device.CreateBuffer<uint32_t>(
        "multi_bindless_array_readback_output",
        4u,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
            EBufferUsageFlags::TRANSFER_SRC
    );

    auto pipeline = ShaderManager::Get().Compute<BindlessTextureReadbackPipeline>(
        "tests/BindlessTextureReadback.comp.hlsl"
    );

    auto texture_a = device.CreateTexture(
        "multi_bindless_array_texture_a",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );
    auto texture_b = device.CreateTexture(
        "multi_bindless_array_texture_b",
        Extent2D(1u, 1u),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::SAMPLED
    );

    auto data_a = MakeSolidRgba8(1u, 1u, 11u);
    auto data_b = MakeSolidRgba8(1u, 1u, 203u);
    const Sampler sampler{SF_NEAREST, SAM_CLAMP_TO_EDGE};

    const uint32_t handle_a = bindless_array_a->AllocateTexture(texture_a->GetView(), sampler);
    const uint32_t handle_b = bindless_array_b->AllocateTexture(texture_b->GetView(), sampler);

    if (handle_a != handle_b) {
        LOG_ERROR(
            "Multi-bindless-array test requires identical local handles, got handle_a={}, handle_b={}",
            handle_a,
            handle_b
        );
        return 1;
    }

    {
        CommandList cmd(EQueueType::Graphics);
        cmd.CopyFrom(ToByteSpan(data_a), texture_a->GetView());
        cmd.CopyFrom(ToByteSpan(data_b), texture_b->GetView());
        cmd.ClearResource(output->GetView(), 0u);

        BindlessTextureReadbackArgs args_a{
            .handle0 = handle_a,
            .handle1 = handle_a,
            .output_offset = 0u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        BindlessTextureReadbackArgs args_b{
            .handle0 = handle_b,
            .handle1 = handle_b,
            .output_offset = 1u,
            .sample_count = 1u,
            .uv0_x = 0.5f,
            .uv0_y = 0.5f,
            .uv1_x = 0.5f,
            .uv1_y = 0.5f,
            .mip0 = 0.0f,
            .mip1 = 0.0f,
        };
        BindlessTextureReadbackArgs args_a_again = args_a;
        args_a_again.output_offset = 2u;

        cmd.UpdateBindlessArray(bindless_array_a);
        cmd.Compute(pipeline, args_a, output->GetView(), bindless_array_a)
            .Dispatch(1u, "MultiBindlessArrayDispatchA");

        cmd.UpdateBindlessArray(bindless_array_b);
        cmd.Compute(pipeline, args_b, output->GetView(), bindless_array_b)
            .Dispatch(1u, "MultiBindlessArrayDispatchB");

        cmd.UpdateBindlessArray(bindless_array_a);
        cmd.Compute(pipeline, args_a_again, output->GetView(), bindless_array_a)
            .Dispatch(1u, "MultiBindlessArrayDispatchARepeat");

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    }

    std::vector<uint32_t> readback_values(4u, 0u);
    {
        CommandList readback_cmd(EQueueType::Graphics);
        GraphEventRef readback_event =
            readback_cmd.ReadbackCopy(output->GetView(), ToByteSpan(readback_values));

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(readback_cmd));
        RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
        if (readback_event) {
            readback_event->Wait();
        }
    }

    constexpr std::array<uint32_t, 3> expected{11u, 203u, 11u};
    for (size_t i = 0; i < expected.size(); ++i) {
        if (readback_values[i] != expected[i]) {
            LOG_ERROR(
                "MultiBindlessArray mismatch at index={}, expected={}, got={}",
                i,
                expected[i],
                readback_values[i]
            );
            return 1;
        }
    }

    LOG_INFO(
        "Multi-bindless-array readback test passed, shared_local_handle={}, results=({}, {}, {})",
        handle_a,
        readback_values[0],
        readback_values[1],
        readback_values[2]
    );
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

int RunGpuEventStreamHierarchyTest() {
    auto& device = RenderDevice::Get();
    GPUEventStream::Get().ResetForTesting();

    auto buffer = device.CreateBuffer<uint32_t>(
        "gpu_event_stream_buffer",
        kElementCount,
        EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::TRANSFER_SRC
    );

    std::vector<uint32_t> readback_values(kElementCount, 0u);
    GraphEventRef readback_event{nullptr};

    CommandList graphics_cmd(EQueueType::Graphics);
    {
        GPU_PROFILE_EVENT_SCOPE(graphics_cmd, "FrameOuter");
        graphics_cmd.ClearResource(buffer->GetView(), 0x11u);
        {
            GPU_PROFILE_EVENT_SCOPE(graphics_cmd, "FrameInner");
            graphics_cmd.ClearResource(buffer->GetView(), 0x55u);
        }
    }
    readback_event =
        graphics_cmd.ReadbackCopy(buffer->GetView(), ToByteSpan(readback_values));
    graphics_cmd.TickFrame();

    Array<CommandList> frame_cmds{};
    frame_cmds.emplace_back(std::move(graphics_cmd));
    RHIExecutor::Get().Submit(std::move(frame_cmds), ERHIExecSubmitFlags::FlushGPU);
    if (readback_event) {
        readback_event->Wait();
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (!ValidateUniformValue(0u, 0x55u, readback_values)) {
        return 1;
    }

    const std::string frame_text = GPUEventStream::Get().FormatLastResolvedFrame();
    if (frame_text.empty()) {
        LOG_ERROR("GPUEventStream did not resolve any frame profile text");
        return 1;
    }

    const std::array<std::string_view, 6> required_tokens{
        "Frame ",
        "Queue Graphics",
        "GPU [queue=Graphics",
        "FrameOuter",
        "FrameInner",
        "exclusive_ns=",
    };
    for (std::string_view token : required_tokens) {
        if (frame_text.find(token) == std::string::npos) {
            LOG_ERROR("GPUEventStream frame text missing token '{}':\n{}", token, frame_text);
            return 1;
        }
    }

    const size_t outer_pos = frame_text.find("FrameOuter");
    const size_t inner_pos = frame_text.find("FrameInner");
    if (outer_pos == std::string::npos || inner_pos == std::string::npos || outer_pos >= inner_pos) {
        LOG_ERROR("GPUEventStream frame hierarchy order is invalid:\n{}", frame_text);
        return 1;
    }

    LOG_INFO("GPUEventStream frame debug:\n{}", frame_text);
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
        graphics_cmd.TickFrame();

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU,
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
        graphics_cmd.TickFrame();

        Array<CommandList> frame_cmds{};
        frame_cmds.emplace_back(std::move(graphics_cmd));

        RHIPresentRequest present_request{swapchain, output->GetView()};
        RHIExecutor::Get().Submit(
            std::move(frame_cmds),
            ERHIExecSubmitFlags::FlushGPU,
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

        auto shutdown_and_return = [&](int code) {
            if (window_inited) {
                WindowContext::ShutDown();
                window_inited = false;
            }
            ShaderManager::ShutDown();
            ShutdownRHIForTest();
            Moer::TaskSystem::ShutDown();
            return code;
        };

        const int queue_ret = RunNamedTestCase("CommandListQueueBinding", RunCommandListQueueBindingTest);
        if (queue_ret != 0) {
            return shutdown_and_return(queue_ret);
        }

        const int translate_metadata_ret = RunNamedTestCase(
            "TranslateExecutionMetadataRoundTrip",
            RunTranslateExecutionClassRoundTripTest
        );
        if (translate_metadata_ret != 0) {
            return shutdown_and_return(translate_metadata_ret);
        }

        const int translate_lambda_ret =
            RunNamedTestCase("TranslateLambdaCommand", RunTranslateLambdaCommandTest);
        if (translate_lambda_ret != 0) {
            return shutdown_and_return(translate_lambda_ret);
        }

        const int translate_readback_ret =
            RunNamedTestCase("MultiQueueReadback", RunRHITranslateMultiQueueReadbackTest);
        if (translate_readback_ret != 0) {
            return shutdown_and_return(translate_readback_ret);
        }

        const int multi_cmd_order_ret =
            RunNamedTestCase("MultiCommandListSubmitOrdering", RunMultiCommandListSubmitOrderingTest);
        if (multi_cmd_order_ret != 0) {
            return shutdown_and_return(multi_cmd_order_ret);
        }

        const int serial_control_ret = RunNamedTestCase(
            "SerialControlTranslateOrdering",
            RunSerialControlTranslateOrderingTest
        );
        if (serial_control_ret != 0) {
            return shutdown_and_return(serial_control_ret);
        }

        const int descriptor_heap_ret = RunNamedTestCase(
            "ConcurrentDescriptorRangeAllocation",
            RunDescriptorHeapConcurrentRangeAllocationTest
        );
        if (descriptor_heap_ret != 0) {
            return shutdown_and_return(descriptor_heap_ret);
        }

        const int graphics_copyscope_ret =
            RunNamedTestCase("GraphicsCopyScopeRoundTrip", RunGraphicsCopyScopeRoundTripTest);
        if (graphics_copyscope_ret != 0) {
            return shutdown_and_return(graphics_copyscope_ret);
        }

        const int bindless_readback_ret =
            RunNamedTestCase("BindlessBufferReadback", RunBindlessBufferReadbackTest);
        if (bindless_readback_ret != 0) {
            return shutdown_and_return(bindless_readback_ret);
        }

        const int bindless_texture_readback_ret =
            RunNamedTestCase("BindlessTextureReadback", RunBindlessTextureReadbackTest);
        if (bindless_texture_readback_ret != 0) {
            return shutdown_and_return(bindless_texture_readback_ret);
        }

        const int multi_bindless_array_ret =
            RunNamedTestCase("MultiBindlessArrayReadback", RunMultiBindlessArrayReadbackTest);
        if (multi_bindless_array_ret != 0) {
            return shutdown_and_return(multi_bindless_array_ret);
        }

        const int multi_scope_ret =
            RunNamedTestCase("MultiCopyScopeOrdering", RunMultiCopyScopeOrderingTest);
        if (multi_scope_ret != 0) {
            return shutdown_and_return(multi_scope_ret);
        }

        const int unknown_first_use_ret =
            RunNamedTestCase("CopyScopeUnknownFirstUse", RunCopyScopeUnknownFirstUseTest);
        if (unknown_first_use_ret != 0) {
            return shutdown_and_return(unknown_first_use_ret);
        }

        const int gpu_event_stream_ret =
            RunNamedTestCase("GPUEventStreamHierarchy", RunGpuEventStreamHierarchyTest);
        if (gpu_event_stream_ret != 0) {
            return shutdown_and_return(gpu_event_stream_ret);
        }

        WindowContext::Init(SurfaceInitInfo(ERHIType::Vulkan, 640, 360, "TestRHITranslatePresent", false));
        window_inited = true;
        const int present_copyscope_ret =
            RunNamedTestCase("PresentWithCopyScope", RunPresentWithCopyScopeTests);
        if (present_copyscope_ret != 0) {
            return shutdown_and_return(present_copyscope_ret);
        }
        const int present_ret = RunNamedTestCase("PresentRoundTrip", RunPresentTests);
        if (present_ret != 0) {
            return shutdown_and_return(present_ret);
        }
        return shutdown_and_return(0);
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
