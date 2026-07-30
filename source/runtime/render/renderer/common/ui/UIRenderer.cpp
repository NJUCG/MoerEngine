// 将上层 UI 生命周期调用转发到 Dear ImGui 后端，并维护跨线程帧数据的后端所有权。

#include "renderer/common/UIRenderer.h"
#include "DearImGuiRenderer.h"
#include "rhi/RHI.h"

namespace Moer::Render {
void BindUiViewportWindowFrame(
    UiViewportDrawPacket&      viewport,
    const WindowFrameSnapshot& window_frame
) noexcept {
    viewport.window_frame = window_frame;
    if (!window_frame.IsDrawable() ||
        window_frame.logical_extent.x == 0 ||
        window_frame.logical_extent.y == 0) {
        return;
    }
    viewport.framebuffer_scale = float2(
        static_cast<float>(window_frame.drawable_extent.x) /
            static_cast<float>(window_frame.logical_extent.x),
        static_cast<float>(window_frame.drawable_extent.y) /
            static_cast<float>(window_frame.logical_extent.y)
    );
}

void BindUiViewportPresentation(
    UiViewportDrawPacket&                 viewport,
    const UiViewportPresentationSnapshot& presentation
) noexcept {
    viewport.presentation = presentation;
    if (!presentation.IsValid() ||
        !viewport.window_frame.IsValid() ||
        presentation.surface_identity != viewport.window_frame.surface_identity ||
        presentation.drawable_generation != viewport.window_frame.drawable_generation ||
        viewport.window_frame.logical_extent.x == 0 ||
        viewport.window_frame.logical_extent.y == 0) {
        return;
    }
    viewport.framebuffer_scale = float2(
        static_cast<float>(presentation.drawable_extent.x) /
            static_cast<float>(viewport.window_frame.logical_extent.x),
        static_cast<float>(presentation.drawable_extent.y) /
            static_cast<float>(viewport.window_frame.logical_extent.y)
    );
}

bool RetargetMainUiPresentation(
    UiViewportDrawPacket&      main_viewport,
    UiCompositionFrameData&    composition,
    const WindowFrameSnapshot& window_frame,
    Extent2D                   committed_extent
) noexcept {
    if (!window_frame.IsDrawable() ||
        window_frame.logical_extent.x == 0 ||
        window_frame.logical_extent.y == 0 ||
        committed_extent.x == 0 ||
        committed_extent.y == 0 ||
        window_frame.drawable_generation == 0) {
        return false;
    }
    if (main_viewport.presentation.IsValid() &&
        main_viewport.presentation.surface_identity == window_frame.surface_identity &&
        main_viewport.presentation.drawable_generation == window_frame.drawable_generation &&
        main_viewport.presentation.drawable_extent == committed_extent) {
        return true;
    }

    const float2 committed_to_raw_scale(
        static_cast<float>(committed_extent.x) /
            static_cast<float>(window_frame.drawable_extent.x),
        static_cast<float>(committed_extent.y) /
            static_cast<float>(window_frame.drawable_extent.y)
    );

    BindUiViewportWindowFrame(main_viewport, window_frame);
    BindUiViewportPresentation(
        main_viewport,
        UiViewportPresentationSnapshot{
            .surface_identity    = window_frame.surface_identity,
            .drawable_extent     = committed_extent,
            .drawable_generation = window_frame.drawable_generation,
        }
    );

    if (composition.scene_extent_resolved && !composition.separate_window) {
        composition.output_resolution = uint2(committed_extent.x, committed_extent.y);
        composition.scene_color_position.x *= committed_to_raw_scale.x;
        composition.scene_color_position.y *= committed_to_raw_scale.y;
        composition.scene_color_resolution.x *= committed_to_raw_scale.x;
        composition.scene_color_resolution.y *= committed_to_raw_scale.y;
    }
    return true;
}

bool ConvertUiClipRectToDrawable(
    UiClipRect&   clip_rect,
    const float2& display_position,
    const float2& framebuffer_scale
) noexcept {
    if (framebuffer_scale.x <= 0.f || framebuffer_scale.y <= 0.f) {
        return false;
    }
    clip_rect.min.x = (clip_rect.min.x - display_position.x) * framebuffer_scale.x;
    clip_rect.min.y = (clip_rect.min.y - display_position.y) * framebuffer_scale.y;
    clip_rect.max.x = (clip_rect.max.x - display_position.x) * framebuffer_scale.x;
    clip_rect.max.y = (clip_rect.max.y - display_position.y) * framebuffer_scale.y;
    return clip_rect.max.x > clip_rect.min.x && clip_rect.max.y > clip_rect.min.y;
}

bool IsUiViewportPresentationCommitted(const UiViewportDrawPacket& viewport) noexcept {
    if (!viewport.window_frame.IsValid() ||
        !viewport.presentation.IsValid() ||
        !viewport.framebuffer ||
        !viewport.swapchain ||
        !viewport.swapchain->IsPresentationReady()) {
        return false;
    }
    const auto framebuffer_extent = viewport.framebuffer->GetExtent();
    return viewport.presentation.surface_identity == viewport.window_frame.surface_identity &&
           viewport.presentation.drawable_generation ==
               viewport.window_frame.drawable_generation &&
           viewport.swapchain->GetCommittedSurfaceIdentity() ==
               viewport.presentation.surface_identity &&
           viewport.swapchain->size == viewport.presentation.drawable_extent &&
           framebuffer_extent.x == viewport.presentation.drawable_extent.x &&
           framebuffer_extent.y == viewport.presentation.drawable_extent.y;
}

bool IsUiViewportPresentationCurrent(const UiViewportDrawPacket& viewport) noexcept {
    return !viewport.presentation_metadata_only &&
           viewport.window_frame.IsDrawable() &&
           IsUiViewportPresentationCommitted(viewport);
}

bool ResolveUiCompositionDrawableMetrics(
    UiCompositionFrameData&    composition,
    const WindowFrameSnapshot& main_window_frame,
    const UiDrawFramePacket&   draw_frame
) noexcept {
    composition.scene_extent_resolved = false;
    if (!composition.enabled) {
        return true;
    }

    const WindowFrameSnapshot* target_frame        = &main_window_frame;
    Extent2D                   target_extent       = main_window_frame.drawable_extent;
    float2                     drawable_scale{};
    bool                       target_metadata_only = false;
    if (composition.separate_window) {
        target_frame = nullptr;
        if (!composition.window_frame_buffer) {
            return false;
        }
        for (const UiViewportDrawPacket& viewport : draw_frame.platform_viewports) {
            if (viewport.framebuffer.Get() == composition.window_frame_buffer.Get()) {
                if (!IsUiViewportPresentationCommitted(viewport)) {
                    return false;
                }
                target_frame = &viewport.window_frame;
                target_extent = viewport.presentation.drawable_extent;
                drawable_scale = viewport.framebuffer_scale;
                target_metadata_only = viewport.presentation_metadata_only;
                break;
            }
        }
    }

    if (target_frame == nullptr || !target_frame->IsValid() ||
        (!composition.separate_window && !target_frame->IsDrawable()) ||
        target_frame->logical_extent.x == 0 || target_frame->logical_extent.y == 0) {
        return false;
    }

    if (!composition.separate_window) {
        drawable_scale = float2(
            static_cast<float>(target_extent.x) /
                static_cast<float>(target_frame->logical_extent.x),
            static_cast<float>(target_extent.y) /
                static_cast<float>(target_frame->logical_extent.y)
        );
    }
    composition.output_resolution = uint2(target_extent.x, target_extent.y);
    composition.scene_color_position.x *= drawable_scale.x;
    composition.scene_color_position.y *= drawable_scale.y;
    composition.scene_color_resolution.x *= drawable_scale.x;
    composition.scene_color_resolution.y *= drawable_scale.y;
    composition.scene_extent_resolved = true;
    if (target_metadata_only) {
        composition.enabled             = false;
        composition.window_frame_buffer = nullptr;
    }
    return true;
}

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
    const WindowInputSourceSnapshot& GetInputSnapshot() const noexcept {
        return backend->GetInputSnapshot();
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

const WindowInputSourceSnapshot& UIRenderer::GetInputSnapshot() const noexcept {
    return impl->GetInputSnapshot();
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
