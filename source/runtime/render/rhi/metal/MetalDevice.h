#pragma once

#include "taskgraph/Event.h"
#include "rhi/RHIImpl.h"

#include <memory>

namespace Moer::Render {

// Initial Metal RHI slice: device, 2D color texture, window surface, and Present.
// Unsupported resource and command types fail explicitly until their translators exist.
class MetalDevice final : public RenderDevice::Impl {
public:
    MetalDevice();
    ~MetalDevice() override;

    FenceRef CreateFence() override;
    BufferRef CreateBuffer(std::string_view, uint, uint, EBufferUsageFlags, EPixelFormat) override;
    TextureRef CreateTexture(std::string_view, const TextureInfo&) override;
    BindlessArrayRef CreateBindlessArray(uint) override;
    RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo&) override;
    RaytracingSceneRef CreateRaytracingScene() override;
    CommandQueue& GetCommandQueue(EQueueType) override;
    CopyQueue& GetCopyQueue() override;
    RHIQueueTopology GetQueueTopology() const override;
    SwapchainRef CreateSwapchain(const SwapchainCreateInfo&) override;
    PipelineHandle CreatePipeline(GfxPsoCreateInfo&&, PipelineShaderInfo&&) override;
    PipelineHandle CreatePipeline(PipelineShaderInfo&&) override;
    void WaitIdle() override;

private:
    struct Native;
    std::unique_ptr<Native> native_;
};

// Internal smoke-test bridges for GPU readback and bindless argument-buffer
// checks. They do not expose native Metal objects to renderer passes.
RENDER_API void* GetMetalNativeTexture(Texture* texture) noexcept;
RENDER_API void* GetMetalNativeBuffer(Buffer* buffer) noexcept;
RENDER_API void* GetMetalNativeBindlessIndexBuffer(BindlessArray* array) noexcept;
RENDER_API void* GetMetalNativeBindlessArgumentBuffer(BindlessArray* array, uint set) noexcept;
RENDER_API void* GetMetalNativeComputePipeline(PipelineHandle pipeline) noexcept;

} // namespace Moer::Render
