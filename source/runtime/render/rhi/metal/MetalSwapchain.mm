#include "rhi/metal/MetalSwapchain.h"

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

namespace Moer::Render {

MetalSwapchain::MetalSwapchain(id<MTLDevice> device, const SwapchainCreateInfo& info) : device_(device) {
    Recreate(info);
}

MetalSwapchain::~MetalSwapchain() { Detach(); }

bool MetalSwapchain::Recreate(const SwapchainCreateInfo& info) {
    if (!info.surface.IsValid() || info.size.x == 0 || info.size.y == 0 ||
        ![NSThread isMainThread]) {
        return false;
    }
    const WindowNativeHandle handle = info.surface.source->GetNativeWindow();
    if (handle.window_system != EWindowSystemType::GLFW) return false;
    auto* glfw_window = reinterpret_cast<GLFWwindow*>(handle.window_system_handle);
    NSWindow* window = glfwGetCocoaWindow(glfw_window);
    NSView* view = window.contentView;
    if (view == nil) return false;

    const MTLPixelFormat pixel_format = ToMetalFormat(info.preferred_format);
    const WindowSurfaceIdentity identity = info.surface.GetIdentity();
    if (view_ != view || identity_ != identity || info.force_surface_recreate) {
        Detach();
        view_ = view;
        previous_layer_ = view.layer;
        previous_wants_layer_ = view.wantsLayer;
        layer_ = [CAMetalLayer layer];
        layer_.device = device_;
        layer_.framebufferOnly = NO; // Present copies the RHI framebuffer to the drawable.
        view.wantsLayer = YES;
        view.layer = layer_;
    }
    layer_.pixelFormat = pixel_format;
    layer_.frame = view.bounds;
    layer_.contentsScale = window.backingScaleFactor;
    layer_.drawableSize = CGSizeMake(info.size.x, info.size.y);
    source_ = info.surface.source;
    identity_ = identity;
    format = info.preferred_format;
    size = info.size;
    return true;
}

bool MetalSwapchain::IsPresentationReady() const noexcept { return layer_ != nil && source_ != nullptr; }

WindowSurfaceIdentity MetalSwapchain::GetCommittedSurfaceIdentity() const noexcept { return identity_; }

id<CAMetalDrawable> MetalSwapchain::NextDrawable() const { return [layer_ nextDrawable]; }

void MetalSwapchain::Detach() {
    if (view_ != nil && view_.layer == layer_) {
        view_.layer = previous_layer_;
        view_.wantsLayer = previous_wants_layer_;
    }
    layer_ = nil;
    view_ = nil;
    previous_layer_ = nil;
    source_.reset();
    identity_ = {};
}

} // namespace Moer::Render
