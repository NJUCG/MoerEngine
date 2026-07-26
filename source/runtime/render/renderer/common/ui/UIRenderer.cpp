// 将上层 UI 生命周期调用转发到 Dear ImGui 后端，并维护跨线程帧数据的后端所有权。

#include "renderer/common/UIRenderer.h"
#include "DearImGuiRenderer.h"
#include "rhi/RHI.h"

namespace Moer::Render {
UiDrawFrameSlotClaim::UiDrawFrameSlotClaim(UiDrawFramePacket& frame) {
    slots.reserve(frame.platform_viewports.size() + 1);
    Freeze(frame.main_viewport);
    for (UiViewportDrawPacket& viewport : frame.platform_viewports) {
        Freeze(viewport);
    }
}

void UiDrawFrameSlotClaim::Freeze(UiViewportDrawPacket& viewport) {
    if (viewport.commands.empty()) {
        return;
    }
    if (!viewport.render_resources) {
        structurally_valid = false;
        return;
    }

    const uint64_t pending_slot =
        viewport.render_resources->GetPendingRecordingSlot();
    viewport.recording_slot = pending_slot;

    for (const Slot& slot : slots) {
        if (slot.resources.get() != viewport.render_resources.get()) {
            continue;
        }
        if (slot.value != pending_slot) {
            structurally_valid = false;
        }
        return;
    }
    slots.emplace_back(Slot{
        .resources = viewport.render_resources,
        .value     = pending_slot,
    });
}

bool UiDrawFrameSlotClaim::ValidatePendingSlots() const noexcept {
    if (!structurally_valid) {
        return false;
    }
    for (const Slot& slot : slots) {
        if (!slot.resources ||
            !slot.resources->IsPendingRecordingSlot(slot.value)) {
            return false;
        }
    }
    return true;
}

bool UiDrawFrameSlotClaim::IsReadyForRecording() const noexcept {
    return GetState() == EState::Pending && ValidatePendingSlots();
}

bool UiDrawFrameSlotClaim::CommitAccepted() const noexcept {
    EState current = GetState();
    if (current == EState::Accepted) {
        return true;
    }
    if (current != EState::Pending || !ValidatePendingSlots()) {
        EState expected = EState::Pending;
        state.compare_exchange_strong(
            expected,
            EState::Rejected,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        );
        return false;
    }

    EState expected = EState::Pending;
    if (!state.compare_exchange_strong(
            expected,
            EState::Accepted,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        return expected == EState::Accepted;
    }
    for (const Slot& slot : slots) {
        slot.resources->CommitRecordingSlot(slot.value);
    }
    return true;
}

void UiDrawFrameSlotClaim::Reject() const noexcept {
    EState expected = EState::Pending;
    state.compare_exchange_strong(
        expected,
        EState::Rejected,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

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
    const UiDrawFramePacket& _frame,
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
