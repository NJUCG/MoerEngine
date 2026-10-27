#include "renderer/raster/CudaVulkanTools.h"
#include "rhi/RHICommand.h"
#include "rhi/executor/RHIExecutor.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Moer;
using namespace Moer::Render;

namespace {
void Check(cudaError_t result) {
    if (result != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(result));
}

void RoundTrip(EPixelFormat format, ETextureUsageFlags usage, bool cuda_write) {
    auto&          device = RenderDevice::Get();
    constexpr uint width = 64, height = 32;
    // Keep a neighbouring allocation live to exercise nonzero VMA offsets.
    auto neighbour = device.CreateTexture("CudaInteropNeighbour", Extent3D(width, height, 1), format, usage);
    auto texture   = device.CreateTexture("CudaInteropReadback", Extent3D(width, height, 1), format, usage);
    Raster::CudaTexture         imported(device, texture);
    Raster::CudaInteropBoundary boundary(device, {texture});
    cudaArray_t                 array{};
    Check(cudaGetMipmappedArrayLevel(&array, imported.cudaMipmappedImageArray, 0));
    const auto undefined = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_NONE, ERHIAccessFlags::UNDEFINED, ETextureLayout::TEXTURE_LAYOUT_UNDEFINED
    );
    const auto transfer_write = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_WRITE,
        ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST
    );
    const auto transfer_read = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_TRANSFER,
        ERHIAccessFlags::TRANSFER_READ,
        ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC
    );
    const auto sampled = BarrierState::Texture(
        ERHIPipelineStageFlags::PS_ALL_COMMANDS,
        ERHIAccessFlags::SHADER_READ,
        ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );
    const auto          view   = texture->GetView();
    const auto          aspect = texture->GetAspectFlags();
    std::vector<uint32> expected(width * height), actual(width * height);
    for (uint iteration = 0; iteration != 3; ++iteration) {
        for (size_t i = 0; i != expected.size(); ++i)
            expected[i] = 0x3e800000u + uint32(i) + iteration;
        CommandList upload;
        upload.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
        upload.Barriers(std::array{
            BarrierCreateInfo::Transition(view, iteration ? sampled : undefined, transfer_write, aspect)
        });
        upload.CopyFrom(
            std::span<const Moer::byte>(
                reinterpret_cast<const Moer::byte*>(expected.data()), expected.size() * sizeof(uint32)
            ),
            view
        );
        upload.Barriers(std::array{BarrierCreateInfo::Transition(view, transfer_write, sampled, aspect)});
        RHIExecutor::Get().Submit(EQueueType::Graphics, upload.Submit(), ERHIExecSubmitFlags::FlushGPU);

        boundary.ReleaseToCuda();
        Check(cudaMemcpy2DFromArrayAsync(
            actual.data(),
            width * sizeof(uint32),
            array,
            0,
            0,
            width * sizeof(uint32),
            height,
            cudaMemcpyDeviceToHost,
            boundary.stream_to_run
        ));
        Check(cudaStreamSynchronize(boundary.stream_to_run));
        if (actual != expected)
            throw std::runtime_error("Vulkan writes are not visible to CUDA");
        if (cuda_write) {
            for (auto& value : expected)
                value += 17;
            Check(cudaMemcpy2DToArrayAsync(
                array,
                0,
                0,
                expected.data(),
                width * sizeof(uint32),
                width * sizeof(uint32),
                height,
                cudaMemcpyHostToDevice,
                boundary.stream_to_run
            ));
        }
        boundary.AcquireFromCuda();

        std::fill(actual.begin(), actual.end(), 0xdeadbeefu);
        CommandList readback;
        readback.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
        readback.Barriers(std::array{BarrierCreateInfo::Transition(view, sampled, transfer_read, aspect)});
        // Owning texture readbacks intentionally exclude depth formats. Keep the
        // destination alive until RHI Sync has retired the copy callback.
        readback.CopyFrom(
            view,
            std::span<Moer::byte>(
                reinterpret_cast<Moer::byte*>(actual.data()), actual.size() * sizeof(uint32)
            )
        );
        readback.Barriers(std::array{BarrierCreateInfo::Transition(view, transfer_read, sampled, aspect)});
        RHIExecutor::Get().Submit(EQueueType::Graphics, readback.Submit(), ERHIExecSubmitFlags::FlushGPU);
        // Sync drains the completion callbacks as well as the GPU submission.
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (actual != expected)
            throw std::runtime_error("Vulkan readback after CUDA ownership does not match");
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    std::cout << "[TESTCASE][PASS] CudaVulkanRoundTrip format=" << uint32(format)
              << " cuda_write=" << cuda_write << " iterations=3 pixels=" << width * height
              << " readback=verified\n";
}
} // namespace

namespace Moer::Render::Testing {
RENDER_API void RunCudaInteropReadbacks() {
    const auto usage =
        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST | ETextureUsageFlags::TRANSFER_SRC;
    RoundTrip(PF_R8G8B8A8_UNORM, usage | ETextureUsageFlags::COLOR_ATTACHMENT, true);
    // The denoiser samples depth in CUDA; only the color output is CUDA-written.
    RoundTrip(PF_D32_SFLOAT, usage | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT, false);
}
} // namespace Moer::Render::Testing
