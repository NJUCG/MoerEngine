#include "renderer/common/PresentationSurface.h"

#include <cassert>

namespace Moer::Render {

PresentationSurface::PresentationSurface(RenderDevice& in_device, PresentationSurfaceDesc desc) :
    device(in_device),
    window(desc.window),
    swapchain_info{
        .surface          = Moer::WindowContext::CreateSwapchainSurfaceInfo(window),
        .size             = desc.size,
        .back_buffer_sz   = desc.back_buffer_count,
        .preferred_format = desc.preferred_format,
    },
    debug_name(std::move(desc.debug_name)) {
    if (swapchain_info.size.x == 0 || swapchain_info.size.y == 0) {
        return;
    }

    swapchain = device.CreateSwapchain(swapchain_info);
}

void PresentationSurface::Resize(Extent2D new_size) {
    const Extent2D previous_size = swapchain_info.size;
    swapchain_info.size = new_size;

    if (new_size.x == 0 || new_size.y == 0) {
        frame_buffer = nullptr;
        return;
    }

    if (swapchain && previous_size.x == new_size.x && previous_size.y == new_size.y) {
        return;
    }

    frame_buffer        = nullptr;
    if (!swapchain) {
        swapchain = device.CreateSwapchain(swapchain_info);
        return;
    }

    swapchain->Sync();
    swapchain->Recreate(swapchain_info);
}

void PresentationSurface::Sync() {
    if (swapchain) {
        swapchain->Sync();
    }
}

RHIPresentRequest PresentationSurface::CreatePresentRequest(TextureView source) const {
    assert(IsPresentable() && "PresentationSurface cannot present without a presentable swapchain");
    assert(source.GetTexture() && "PresentationSurface present source is null");

    const uint3 source_extent = source.GetTexture()->GetExtent();
    assert(source_extent.x == swapchain_info.size.x && source_extent.y == swapchain_info.size.y &&
           "PresentationSurface present source extent must match the swapchain extent");

    return RHIPresentRequest{swapchain, source};
}

TextureRef PresentationSurface::EnsureFrameBuffer(EPixelFormat format, ETextureUsageFlags usage) {
    if (!swapchain || swapchain_info.size.x == 0 || swapchain_info.size.y == 0) {
        frame_buffer = nullptr;
        return nullptr;
    }

    if (frame_buffer) {
        const uint3 extent = frame_buffer->GetExtent();
        if (extent.x == swapchain_info.size.x && extent.y == swapchain_info.size.y &&
            frame_buffer->GetFormat() == format) {
            return frame_buffer;
        }
    }

    frame_buffer = device.CreateTexture(
        debug_name,
        Extent2D(swapchain_info.size.x, swapchain_info.size.y),
        format,
        usage
    );
    frame_buffer->SetName(debug_name);
    return frame_buffer;
}

TextureView PresentationSurface::GetFrameBufferView() const {
    return frame_buffer ? frame_buffer->GetView() : TextureView();
}

void PresentationSurface::ClearFrameBuffer() {
    frame_buffer = nullptr;
}

SwapchainRef PresentationSurface::GetSwapchain() const {
    return swapchain;
}

EPixelFormat PresentationSurface::GetFormat() const {
    return swapchain ? swapchain->format : swapchain_info.preferred_format;
}

Extent2D PresentationSurface::GetSize() const {
    return swapchain_info.size;
}

bool PresentationSurface::IsPresentable() const {
    return swapchain && swapchain_info.size.x > 0 && swapchain_info.size.y > 0;
}

} // namespace Moer::Render
