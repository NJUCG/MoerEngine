#pragma once

#include "rhi/metal/MetalFormat.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace Moer::Render {
class MetalSwapchain final : public Swapchain {
public:
    MetalSwapchain(id<MTLDevice> device, const SwapchainCreateInfo& info);
    ~MetalSwapchain() override;

    bool Recreate(const SwapchainCreateInfo& info) override;
    bool IsPresentationReady() const noexcept override;
    WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override;
    id<CAMetalDrawable> NextDrawable() const;

private:
    void Detach();

    id<MTLDevice> device_;
    CAMetalLayer* layer_{nil};
    NSView* view_{nil};
    CALayer* previous_layer_{nil};
    bool previous_wants_layer_{false};
    WindowSurfaceSourceRef source_{};
    WindowSurfaceIdentity identity_{};
};

} // namespace Moer::Render
