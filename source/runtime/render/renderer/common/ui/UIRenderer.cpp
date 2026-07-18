// 将上层 UI 生命周期调用转发到 Dear ImGui 后端，并维护跨线程帧数据的后端所有权。

#include "renderer/common/UIRenderer.h"
#include "DearImGuiRenderer.h"
#include "rhi/RHI.h"

namespace Moer::Render {
struct UIRenderer::Impl {
    explicit Impl(RenderDevice& _device) : backend(MakeShared<ImGuiRenderBackend>(_device)) {}

    void RegisterImage(Texture* _texture, Sampler _sampler) {
        backend->RegisterImage(_texture, _sampler);
    }
    void UnregisterImage(Texture* _texture) {
        backend->UnregisterImage(_texture);
    }

    void BeginGUIFrame() {
        backend->BeginGUIFrame();
    }
    void EndGUIFrame() {
        backend->EndGUIFrame();
    }
    void UpdatePlatformWindows() {
        backend->UpdatePlatformWindows();
    }

    UiDrawFramePacket CaptureDrawFrame() {
        auto frame    = backend->CaptureDrawFrame();
        frame.backend = backend;
        return frame;
    }
    SharedPtr<ImGuiRenderBackend> backend;
};

UIRenderer::UIRenderer(RenderDevice& _device) : impl(MakeUnique<Impl>(_device)) {}

UIRenderer::~UIRenderer() = default;

void UIRenderer::RegisterImage(Texture* _texture, Sampler _sampler) {
    impl->RegisterImage(_texture, _sampler);
}

void UIRenderer::UnregisterImage(Texture* _texture) {
    impl->UnregisterImage(_texture);
}

void UIRenderer::UnRegisterImage(Texture* _texture) {
    UnregisterImage(_texture);
}

void UIRenderer::BeginGUIFrame() {
    impl->BeginGUIFrame();
}

void UIRenderer::EndGUIFrame() {
    impl->EndGUIFrame();
}

void UIRenderer::UpdatePlatformWindows() {
    impl->UpdatePlatformWindows();
}

UiDrawFramePacket UIRenderer::CaptureDrawFrame() {
    return impl->CaptureDrawFrame();
}

TextureRef UIRenderer::GetWindowFrameBuffer(void* _window) {
    return impl->backend->GetWindowFrameBuffer(_window);
}

void RenderUiDrawFrame(
    CommandList&           _cmd_list,
    const TextureView&     _main_framebuffer,
    UiDrawFramePacket&     _frame,
    EUiDrawExecutionThread _execution_thread
) {
    if (_frame.backend) {
        ScopedGpuMarker ui_marker(
            _cmd_list, "Editor UI", GpuMarkerPalette::Ui()
        );
        _frame.backend->RenderGUI(_cmd_list, _main_framebuffer, _frame, _execution_thread);
    }
}

void PresentUiDrawFrame(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread) {
    if (_frame.backend) {
        _frame.backend->PresentWindows(_frame, _execution_thread);
    }
}

} // namespace Moer::Render
