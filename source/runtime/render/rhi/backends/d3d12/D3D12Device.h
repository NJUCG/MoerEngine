#pragma once

#include "rhi/RHIImpl.h"
#include <memory>

namespace Moer::Render {

RENDER_API uint32 GetD3D12ValidationErrorCount() noexcept;

struct D3D12RHIConfig {
    bool force_sync = false;
    bool debug      = true;
};

class D3D12Device final : public RenderDevice::Impl {
public:
    explicit D3D12Device(D3D12RHIConfig config);
    ~D3D12Device() override;
    FenceRef              CreateFence() override;
    BufferRef             CreateBuffer(std::string_view, uint32, uint32, EBufferUsageFlags, EPixelFormat) override;
    TextureRef            CreateTexture(std::string_view, const TextureInfo&) override;
    BindlessArrayRef      CreateBindlessArray(uint32) override;
    RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo&) override;
    RaytracingSceneRef    CreateRaytracingScene() override;
    CommandQueue&         GetCommandQueue(EQueueType) override;
    CopyQueue&            GetCopyQueue() override;
    RHIQueueTopology      GetQueueTopology() const override;
    SwapchainRef          CreateSwapchain(const SwapchainCreateInfo&) override;
    PipelineHandle        CreatePipeline(GfxPsoCreateInfo&&, PipelineShaderInfo&&) override;
    PipelineHandle        CreatePipeline(PipelineShaderInfo&&) override;
    void                  WaitIdle() override;
    bool                  SupportsTessellation() const override;
    uint32                GetMaxTessellationFactor() const override;
    bool                  SupportsRaytracingAccelerationStructures() const override;
    void                  FlushDebugMessages() const override;

private:
    struct Native;
    std::unique_ptr<Native> native;
};

} // namespace Moer::Render
