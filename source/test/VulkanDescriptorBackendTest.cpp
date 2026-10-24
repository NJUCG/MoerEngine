#include "Core.h"
#include "config/ConfigManager.h"
#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/vulkan/VulkanDescriptorMode.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"

#include <array>
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

void RunDescriptorReadback() {
    auto& device   = RenderDevice::Get();
    auto& shaders  = ShaderManager::Get();
    auto  bound    = shaders.Compute<DescriptorBoundTest>("test/DescriptorBackend.hlsl", "BoundMain");
    auto  bindless = shaders.Compute<DescriptorBindlessTest>("test/DescriptorBackend.hlsl", "BindlessMain");
    auto  arrays   = shaders.Compute<DescriptorArrayTest>("test/DescriptorBackend.hlsl", "ArrayMain");
    const auto& native_device = *static_cast<VulkanDevice*>(device.GetImpl());
    const bool  large_constants =
        native_device.GetCoreProperties().core_1_0.limits.maxPushConstantsSize >= 256;
    auto fallback =
        large_constants ?
            shaders.Compute<DescriptorFallbackTest>("test/DescriptorFallback.hlsl", "BindlessMain") :
            DescriptorFallbackTest{};
    const auto* native_bound    = static_cast<VulkanPipelineState*>(bound.handle.GetState());
    const auto* native_bindless = static_cast<VulkanPipelineState*>(bindless.handle.GetState());
    const auto* native_arrays   = static_cast<VulkanPipelineState*>(arrays.handle.GetState());
    if (native_bound->GetDescriptorMode() != VulkanDescriptorMode::Sets ||
        (native_device.GetDescriptorCapabilities().max_push_descriptors < 40 &&
         native_arrays->binding_template->set_bindings.at(5).push))
        throw std::runtime_error("Ordinary pipeline or over-limit array did not use the set fallback");
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
    CommandList       upload;
    upload.CopyFrom(Bytes(input_value), input->GetView());
    upload.CopyFrom(Bytes(params), uniform->GetView());
    upload.CopyFrom(Bytes(texel), texture->GetView());
    upload.CopyFrom(Bytes(depth_value), depth->GetView());
    upload.UpdateBindlessArray(table);
    RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit(), ERHIExecSubmitFlags::FlushGPU);
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);

    const auto        start = std::chrono::steady_clock::now();
    DescriptorPadding padding{};
    padding.values[13].x = 7;
    std::array<std::array<uint, 192>, 8> readbacks{};
    for (uint batch = 0; batch < 8; ++batch) {
        auto&       readback = readbacks[batch];
        CommandList commands;
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
              << " dispatches=1536 mixed_modes=true arrays=40 batches=8 in_flight=4 large_constants="
              << large_constants << " elapsed_ms=" << elapsed << '\n';
}

int main(int argc, char**) {
    bool tasks = false, device = false, shaders = false;
    int  result = 0;
    try {
        LogSystem::Init();
        ConfigManager::GetInstance().Init(std::filesystem::current_path());
        TaskSystem::Init();
        tasks = true;
        RenderDevice::Init(DeviceInitInfo{
            .rhi_type                               = ERHIType::Vulkan,
            .name                                   = "DescriptorBackendTest",
            .rhi_api_version                        = "1.3",
            .rhi_thread                             = true,
            .rhi_bypass                             = false,
            .parallel_recording                     = argc > 1,
            .parallel_record_workers                = 4,
            .parallel_record_verify                 = argc > 1,
            .parallel_record_min_work_units_per_job = 1,
        });
        device  = true;
        shaders = true;
        RunDescriptorReadback();
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
