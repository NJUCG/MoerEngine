#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/vulkan/VulkanDescriptorMode.h"
#include "rhi/vulkan/VulkanDescriptor.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanPipeline.h"
#include "rhi/vulkan/VulkanTimeline.h"
#include "misc/RAII.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace Moer;
using namespace Moer::Render;

class DescriptorBoundTest : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DescriptorBoundTest);
    DEFINE_SHADER_BUFFER(parameters);
    DEFINE_SHADER_BUFFER(result);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_CONSTANT(uint, iteration);
    DEFINE_SHADER_TEX(depth_source);
    DEFINE_SHADER_ARGS(parameters, result, depth_source, index, iteration);
};
class DescriptorBindlessTest : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DescriptorBindlessTest);
    DEFINE_SHADER_BUFFER(parameters);
    DEFINE_SHADER_BUFFER(result);
    DEFINE_SHADER_BUFFER(typed_source);
    DEFINE_SHADER_TEX(depth_source);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_CONSTANT(uint, iteration);
    DEFINE_SHADER_ARGS(parameters, result, typed_source, depth_source, bdls, index, iteration);
};
class DescriptorArrayTest : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DescriptorArrayTest);
    DEFINE_SHADER_BUFFER_ARRAY(sources, 40);
    DEFINE_SHADER_BUFFER(result);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_CONSTANT(uint, iteration);
    DEFINE_SHADER_ARGS(sources, result, index, iteration);
};
struct DescriptorPadding {
    uint4 values[14];
};
class DescriptorFallbackTest : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(DescriptorFallbackTest);
    DEFINE_SHADER_BUFFER(parameters);
    DEFINE_SHADER_BUFFER(result);
    DEFINE_SHADER_BUFFER(typed_source);
    DEFINE_SHADER_TEX(depth_source);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT(uint, index);
    DEFINE_SHADER_CONSTANT(uint, iteration);
    DEFINE_SHADER_CONSTANT(DescriptorPadding, padding);
    DEFINE_SHADER_ARGS(parameters, result, typed_source, depth_source, bdls, index, iteration, padding);
};

template<class T>
std::span<const Moer::byte> Bytes(const T& value) {
    return {reinterpret_cast<const Moer::byte*>(&value), sizeof(value)};
}

void RunDescriptorReadback(bool test_reservation_pressure) {
    auto& device   = RenderDevice::Get();
    auto& shaders  = ShaderManager::Get();
    auto  bound    = shaders.Compute<DescriptorBoundTest>("test/DescriptorBackend.hlsl", "BoundMain");
    auto  bindless = shaders.Compute<DescriptorBindlessTest>("test/DescriptorBackend.hlsl", "BindlessMain");
    auto  uniform_push = shaders.Compute<DescriptorBindlessTest>("test/DescriptorUniformPush.hlsl", "BindlessMain");
    auto  arrays   = shaders.Compute<DescriptorArrayTest>("test/DescriptorBackend.hlsl", "ArrayMain");
    auto& native_device = *static_cast<VulkanDevice*>(device.GetImpl());
    const bool  large_constants =
        native_device.GetCoreProperties().core_1_0.limits.maxPushConstantsSize >= 256;
    auto fallback =
        large_constants ?
            shaders.Compute<DescriptorFallbackTest>("test/DescriptorFallback.hlsl", "BindlessMain") :
            DescriptorFallbackTest{};
    const auto* native_bound    = static_cast<VulkanPipelineState*>(bound.handle.GetState());
    const auto* native_bindless = static_cast<VulkanPipelineState*>(bindless.handle.GetState());
    const auto* native_arrays   = static_cast<VulkanPipelineState*>(arrays.handle.GetState());
    const auto* native_uniform_push = static_cast<VulkanPipelineState*>(uniform_push.handle.GetState());
    if (native_bound->GetDescriptorMode() != native_device.GetDescriptorMode() ||
        native_arrays->GetDescriptorMode() != native_device.GetDescriptorMode() ||
        native_uniform_push->GetDescriptorMode() != native_device.GetDescriptorMode() ||
        native_bindless->GetDescriptorMode() != native_device.GetDescriptorMode())
        throw std::runtime_error("Eligible ordinary and bindless pipelines must use the device backend");
    const auto find_push_set = [](const VulkanPipelineState& pipeline) {
        std::optional<uint> pushed_set;
        for (const auto& [set, binding] : pipeline.binding_template->set_bindings) {
            if (!binding.push)
                continue;
            if (pushed_set)
                throw std::runtime_error("Only one set may use push descriptors");
            pushed_set = set;
            const auto& resources = std::get<VulkanResourceSetBindings>(binding.resource_bindings);
            for (const auto& resource : resources.bindings) {
                if (resource.descriptor_count && resource.descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
                    throw std::runtime_error("Only uniform buffers may use push descriptors");
            }
        }
        return pushed_set;
    };
    if (find_push_set(*native_bound) || find_push_set(*native_bindless) || find_push_set(*native_arrays))
        throw std::runtime_error("Mixed UB/SSBO, texture and texel-buffer sets must use ordinary storage");
    const auto& caps = native_device.GetDescriptorCapabilities();
    const bool expect_uniform_push = caps.push && caps.max_push_descriptors >= 1 &&
        (native_uniform_push->GetDescriptorMode() == VulkanDescriptorMode::Sets ||
         (native_uniform_push->GetDescriptorMode() == VulkanDescriptorMode::Buffer && caps.buffer_push));
    const auto uniform_push_set = find_push_set(*native_uniform_push);
    if (uniform_push_set != (expect_uniform_push ? std::optional<uint>{5} : std::nullopt))
        throw std::runtime_error("A pure UB set with binding gaps must use push descriptors when supported");
    if (large_constants && native_device.GetDescriptorMode() == VulkanDescriptorMode::Heap &&
        native_device.GetDescriptorCapabilities().max_push_data < 272 &&
        static_cast<VulkanPipelineState*>(fallback.handle.GetState())->GetDescriptorMode() !=
            VulkanDescriptorMode::Sets)
        throw std::runtime_error("Heap push-data overflow did not fall back to native descriptor sets");
    constexpr auto usage = EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_SRC |
                           EBufferUsageFlags::TRANSFER_DST;
    auto input = device.CreateBuffer<uint>("descriptor_input", 1, usage | EBufferUsageFlags::TEXTURE_BUFFER);
    auto uniform =
        device.CreateBuffer<uint4>("descriptor_uniform", 1, usage | EBufferUsageFlags::CONSTANT_BUFFER);
    auto output  = device.CreateBuffer<uint>("descriptor_output", 192, usage);
    auto texture = device.CreateTexture(
        "descriptor_texture",
        Extent3D(1, 1, 1),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
    );
    auto depth = device.CreateDepthBuffer(
        "descriptor_depth", Extent2D(1, 1), PF_D32_SFLOAT, 1,
        ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
    );
    const float depth_value = 0.25f;
    auto        table       = device.CreateBindlessArray();
    const uint  input_index = table->AllocateBuffer(input->GetView());
    const uint  texture_index =
        table->AllocateTexture(texture->GetView(), Sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE));
    if (texture_index != input_index + 1)
        throw std::runtime_error("Unexpected fresh bindless slot order");
    Array<BufferView> input_views{input->GetView(PF_R32_UINT)
    }; // The RHI pads the rest of the declared array.
    const uint        input_value = 23;
    const uint        texel       = 0xffffffffu;
    const uint4       params{100, 0, 0, 0};
    const auto undefined = BarrierState::Texture(ERHIPipelineStageFlags::PS_NONE,
        ERHIAccessFlags::UNDEFINED, ETextureLayout::TEXTURE_LAYOUT_UNDEFINED);
    const auto texture_write = BarrierState::Texture(ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_WRITE, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST);
    const auto sampled = BarrierState::Texture(ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
        ERHIAccessFlags::SHADER_READ, ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const auto transfer_write = BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE);
    const auto transfer_read = BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_READ);
    const auto shader_read = BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
        ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::UNIFORM_READ);
    const auto shader_write = BarrierState::Buffer(ERHIPipelineStageFlags::PS_COMPUTE_SHADER,
        ERHIAccessFlags::SHADER_WRITE);
    CommandList       upload;
    upload.Barriers({
        BarrierCreateInfo::Transition(texture->GetView(), undefined, texture_write, ETextureAspectFlags::COLOR),
        BarrierCreateInfo::Transition(depth->GetView(), undefined, texture_write, ETextureAspectFlags::DEPTH_SLICE)});
    upload.CopyFrom(Bytes(input_value), input->GetView());
    upload.CopyFrom(Bytes(params), uniform->GetView());
    upload.CopyFrom(Bytes(texel), texture->GetView());
    upload.CopyFrom(Bytes(depth_value), depth->GetView());
    upload.Barriers({
        BarrierCreateInfo::Transition(input->GetView(), transfer_write, shader_read),
        BarrierCreateInfo::Transition(uniform->GetView(), transfer_write, shader_read),
        BarrierCreateInfo::Transition(texture->GetView(), texture_write, sampled, ETextureAspectFlags::COLOR),
        BarrierCreateInfo::Transition(depth->GetView(), texture_write, sampled, ETextureAspectFlags::DEPTH_SLICE)});
    upload.UpdateBindlessArray(table);
    RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    if (test_reservation_pressure && native_bindless->GetDescriptorMode() != VulkanDescriptorMode::Sets &&
        native_bindless->binding_template->resource_descriptor_allocation_size != 0) {
        auto& heap = native_device.GetGlobalDescriptorHeap();
        auto occupied = heap.CreateDescriptorBufferLease();
        auto release = OnScopeExit([&] { heap.RecycleDescriptorBufferLease(std::move(occupied)); });
        while (heap.ReserveDescriptorRange(occupied, 64 * 1024)) {}
        // Also consume any sub-page holes left by atom-granular reservations.
        const std::vector<std::vector<uint64_t>> atom{{heap.GetStorageAlignment()}};
        while (heap.PrepareDescriptorRecordings(occupied, atom)) {}
        std::atomic<uint> completed{0}, succeeded{0};
        auto done = device.CreateFence();
        CommandList rejected;
        const uint poison = 999;
        rejected.Barriers({BarrierCreateInfo::Transition(input->GetView(), shader_read, transfer_write)});
        rejected.CopyFrom(Bytes(poison), input->GetView());
        rejected.Barriers({BarrierCreateInfo::Transition(input->GetView(), transfer_write, shader_read)});
        rejected.Compute(bindless, uniform, output, input_views[0], depth->GetView(), table, input_index, 0u)
            .Dispatch(uint3(1, 1, 1));
        rejected.AddCallback([&] { ++completed; });
        rejected.AddSuccessCallback([&] { ++succeeded; });
        auto submit = rejected.Submit();
        submit.Signal(done.Get(), 1);
        RHIExecutor::Get().Submit(EQueueType::Graphics, std::move(submit), ERHIExecSubmitFlags::FlushGPU);
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        auto* native_done = ResourceCast(done.Get());
        if (!native_done->IsRejected(1) || native_done->IsFailed() ||
            completed != 1 || succeeded != 0) {
            throw std::runtime_error("Descriptor capacity rejection was not recoverable and exactly once");
        }
        std::cout << "[PASS] descriptor reservation pressure rejected exactly once without failing the fence\n";
        // The readback workload below must still see 23, proving that the
        // poisoned upload in the rejected transaction never reached the GPU.
    }

    const auto        start = std::chrono::steady_clock::now();
    DescriptorPadding padding{};
    padding.values[13].x = 7;
    std::array<std::array<uint, 192>, 8> readbacks{};
    for (uint batch = 0; batch < 8; ++batch) {
        auto&       readback = readbacks[batch];
        CommandList commands;
        commands.Barriers({BarrierCreateInfo::Transition(output->GetView(), transfer_read, shader_write)});
        for (uint i = 0; i < readback.size(); ++i) {
            const uint iteration = batch * uint(readback.size()) + i;
            if (large_constants && i % 4 == 3)
                commands
                    .Compute(
                        fallback,
                        uniform,
                        output,
                        input_views[0],
                        depth->GetView(),
                        table,
                        input_index,
                        iteration,
                        padding
                    )
                    .Dispatch(uint3(1, 1, 1));
            else if (i % 5 == 4)
                commands
                    .Compute(uniform_push, uniform, output, input_views[0], depth->GetView(), table, input_index, iteration)
                    .Dispatch(uint3(1, 1, 1));
            else if (i % 3 == 0)
                commands.Compute(bound, uniform, output, depth->GetView(), input_index, iteration)
                    .Dispatch(uint3(1, 1, 1));
            else if (i % 3 == 1)
                commands
                    .Compute(bindless, uniform, output, input_views[0], depth->GetView(), table, input_index, iteration)
                    .Dispatch(uint3(1, 1, 1));
            else
                commands.Compute(arrays, std::span<BufferView>(input_views), output, input_index, iteration)
                    .Dispatch(uint3(1, 1, 1));
        }
        commands.Barriers({BarrierCreateInfo::Transition(output->GetView(), shader_write, transfer_read)});
        commands.CopyFrom(
            output->GetView(),
            std::span<Moer::byte>(reinterpret_cast<Moer::byte*>(readback.data()), sizeof(readback))
        );
        RHIExecutor::Get().Submit(
            EQueueType::Graphics,
            commands.Submit(),
            batch % 4 == 3 ? ERHIExecSubmitFlags::FlushGPU : ERHIExecSubmitFlags::None
        );
        if (batch % 4 == 3)
            RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }
    for (uint batch = 0; batch < readbacks.size(); ++batch) {
        const auto& readback = readbacks[batch];
        for (uint i = 0; i < readback.size(); ++i) {
            const uint expected = batch * uint(readback.size()) + i +
                                  (large_constants && i % 4 == 3 ? 409 :
                                   i % 5 == 4                    ? 402 :
                                   i % 3 == 0                    ? 101 :
                                   i % 3 == 1                    ? 402 :
                                                                   23);
            if (readback[i] != expected)
                throw std::runtime_error(
                    "Descriptor readback mismatch at " + std::to_string(i) + ": " +
                    std::to_string(readback[i]) + " != " + std::to_string(expected)
                );
        }
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    std::cout << "[PASS] descriptor backend="
              << VulkanDescriptorModeName(native_bindless->GetDescriptorMode())
              << " bound_backend=" << VulkanDescriptorModeName(native_bound->GetDescriptorMode())
              << " array_backend=" << VulkanDescriptorModeName(native_arrays->GetDescriptorMode())
              << " uniform_push=" << uniform_push_set.has_value()
              << " dispatches=1536 arrays=40 batches=8 in_flight=4 large_constants="
              << large_constants << " elapsed_ms=" << elapsed << '\n';
}

int main(int argc, char** argv) {
    bool tasks = false, device = false, shaders = false;
    int  result = 0;
    try {
        bool pressure = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--reservation-pressure") pressure = true;
            else throw std::invalid_argument("Unknown test argument");
        }
        LogSystem::Init();
        ConfigManager::GetInstance().Init(std::filesystem::current_path());
        TaskSystem::Init();
        tasks = true;

        RenderDevice::Init(DeviceInitInfo{
            .rhi_type                               = ERHIType::Vulkan,
            .name                                   = "DescriptorBackendTest",
            .rhi_api_version                        = "1.3",
        });
        device  = true;
        shaders = true;
        RunDescriptorReadback(pressure);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        result = 1;
    }
    if (shaders)
        ShaderManager::ShutDown();
    if (device)
        RenderDevice::Dispose();
    if (tasks)
        TaskSystem::ShutDown();
    return result;
}
