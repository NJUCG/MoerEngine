#pragma once

#include "RenderAPI.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "string/String.h"
#include "window/WindowContext.h"

namespace Moer::Render {

struct PresentationSurfaceDesc {
    Moer::WindowHandle window;
    Extent2D           size;
    uint               back_buffer_count = 2;
    EPixelFormat       preferred_format  = PF_R8G8B8A8_SRGB;
    String             debug_name         = MOER_TEXT("PresentationSurface");
};

class RENDER_API PresentationSurface {
public:
    PresentationSurface(RenderDevice& device, PresentationSurfaceDesc desc);

    void Resize(Extent2D new_size);
    void Sync();

    RHIPresentRequest CreatePresentRequest(TextureView source) const;

    TextureRef EnsureFrameBuffer(EPixelFormat format, ETextureUsageFlags usage);
    TextureView GetFrameBufferView() const;
    void ClearFrameBuffer();

    SwapchainRef GetSwapchain() const;
    EPixelFormat GetFormat() const;
    Extent2D GetSize() const;
    bool IsPresentable() const;

private:
    RenderDevice&        device;
    Moer::WindowHandle   window;
    SwapchainCreateInfo  swapchain_info;
    SwapchainRef         swapchain;
    TextureRef           frame_buffer;
    String               debug_name;
};

} // namespace Moer::Render
