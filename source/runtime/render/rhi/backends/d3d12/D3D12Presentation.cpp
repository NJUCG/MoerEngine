#include "D3D12Backend.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace Moer::Render::D3D12 {
SwapchainImpl::SwapchainImpl(Context& context, const SwapchainCreateInfo& info) :
    context(context) {
    format = info.preferred_format;
    size   = {};
    if (!Recreate(info))
        throw std::runtime_error("D3D12 swapchain creation failed");
}
bool SwapchainImpl::Recreate(const SwapchainCreateInfo& info) {
    if (!info.surface.IsValid() || !info.size.x || !info.size.y)
        return false;
    context.queue->Sync();
    std::lock_guard lock(mutex);
    const auto      native = info.surface.source->GetNativeWindow();
    HWND            window = reinterpret_cast<HWND>(native.platform_window_handle);
    if (!window && native.window_system == EWindowSystemType::GLFW)
        window = glfwGetWin32Window(reinterpret_cast<GLFWwindow*>(native.window_system_handle));
    if (!window)
        return false;
    const bool same = surface.HasSameIdentity(info.surface) && !info.force_surface_recreate && format == info.preferred_format;
    backbuffers.clear();
    const uint32      count         = std::clamp(info.back_buffer_sz, 2u, 16u);
    const DXGI_FORMAT native_format = StorageFormat(info.preferred_format);
    if (swapchain && same) {
        if (FAILED(swapchain->ResizeBuffers(count, info.size.x, info.size.y, native_format, 0)))
            return false;
    } else {
        swapchain.Reset();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width            = info.size.x;
        desc.Height           = info.size.y;
        desc.Format           = native_format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount      = count;
        desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling          = DXGI_SCALING_STRETCH;
        desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> created;
        DX_CHECK_HRESULT(context.factory->CreateSwapChainForHwnd(context.queue->Native(), window, &desc, nullptr, nullptr, &created));
        DX_CHECK_HRESULT(created.As(&swapchain));
        context.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    }
    for (uint32 i = 0; i < count; ++i) {
        ComPtr<ID3D12Resource> buffer;
        DX_CHECK_HRESULT(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)));
        backbuffers.push_back(buffer);
    }
    surface = info.surface;
    format  = info.preferred_format;
    size    = info.size;
    return true;
}
void Queue::Present(SwapchainRef swapchain, TextureView source, PresentReceiptRef receipt) {
    auto resolve = [&](bool success, bool recreate = false) {
        if (receipt)
            receipt->Resolve(success, recreate);
    };
    if (!swapchain || !source.texture) {
        resolve(false);
        return;
    }
    auto& target = *static_cast<SwapchainImpl*>(swapchain.Get());
    if (!target.IsPresentationReady() || source.num_mips != 1 || source.num_array != 1 || source.texture->GetNumSamples() != 1 ||
        StorageFormat(source.format) != StorageFormat(target.format) ||
        std::max(1u, source.texture->GetWidth() >> source.mip_level) != target.size.x ||
        std::max(1u, source.texture->GetHeight() >> source.mip_level) != target.size.y) {
        resolve(false, true);
        return;
    }
    try {
        std::lock_guard submit_lock(submission_mutex);
        std::lock_guard swapchain_lock(target.mutex);
        CmdSubmit       empty{Array<UniquePtr<Command>>{}, Array<std::function<void()>>{}, Array<std::function<void()>>{}, TCachedArgArray{}};
        auto            packet = std::make_unique<Packet>(std::move(empty));
        packet->resources.emplace_back(swapchain.Get());
        packet->resources.emplace_back(source.texture);
        Recorder recorder(context, *packet);
        auto&    src = *static_cast<GpuTexture*>(source.texture);
        recorder.Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, source.mip_level, 1, source.array_layer, 1);
        auto*                  destination = target.backbuffers[target.swapchain->GetCurrentBackBufferIndex()].Get();
        D3D12_RESOURCE_BARRIER barrier     = CD3DX12_RESOURCE_BARRIER::Transition(destination, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        packet->list->ResourceBarrier(1, &barrier);
        const CD3DX12_TEXTURE_COPY_LOCATION src_location(src.resource.Get(), src.Subresource(source.mip_level, source.array_layer));
        const CD3DX12_TEXTURE_COPY_LOCATION dst_location(destination, 0);
        packet->list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        packet->list->ResourceBarrier(1, &barrier);
        DX_CHECK_HRESULT(packet->list->Close());
        SubmitPacket(std::move(packet));
        const HRESULT result = target.swapchain->Present(0, 0);
        if (FAILED(result)) {
            LOG_ERROR("D3D12 Present failed: {:#x}", uint32(result));
            resolve(false, result == DXGI_ERROR_INVALID_CALL);
        } else
            resolve(true);
    } catch (const std::exception& error) {
        LOG_ERROR("D3D12 Present: {}", error.what());
        resolve(false);
    }
}
} // namespace Moer::Render::D3D12
