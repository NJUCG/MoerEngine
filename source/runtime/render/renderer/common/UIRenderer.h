// 定义 UI 绘制帧的跨线程数据包，并为具体的 ImGui 渲染后端提供稳定入口。

#ifndef MOER_ENGINE_UI_RENDERER_H
#define MOER_ENGINE_UI_RENDERER_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "window/WindowFrameSnapshot.h"
#include "window/WindowInput.h"
#include <atomic>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Moer::Render {
class UiDrawFrameBackend;

struct UiCompositionFrameData {
    bool       enabled               = false;
    bool       separate_window       = false;
    bool       scene_extent_resolved = false;
    uint2      output_resolution{};
    float2     scene_color_position{};
    float2     scene_color_resolution{};
    TextureRef window_frame_buffer{};
};

class RENDER_API UiViewportRenderResources {
public:
    virtual ~UiViewportRenderResources() = default;

    /**
     * UI upload-ring selection is speculative until the command source is
     * accepted by the native queue. The renderer records one frame at a time,
     * so a read-only token can be frozen in the copied packet and committed
     * only after Fence::WaitSubmitted succeeds.
     */
    [[nodiscard]] virtual uint64_t GetPendingRecordingSlot() const noexcept = 0;
    [[nodiscard]] virtual bool
    IsPendingRecordingSlot(uint64_t slot) const noexcept = 0;
    virtual void CommitRecordingSlot(uint64_t slot) noexcept = 0;
};

enum class EUiDrawExecutionThread : uint8_t {
    Game,
    Render
};

struct UiDrawVertex {
    float2 position{};
    float2 uv{};
    uint32 color = 0;
};

using UiDrawIndex = uint16;

struct UiDrawCommand {
    // Viewport-local logical coordinates. The execution target's immutable
    // framebuffer scale is applied only when the RT builds the native batch.
    float2 clip_min{};
    float2 clip_max{};
    uint32 texture_handle = 0;
    uint32 element_count  = 0;
    uint32 vertex_offset  = 0;
    uint32 index_offset   = 0;
};

struct UiClipRect {
    float2 min{};
    float2 max{};
};

struct UiViewportPresentationSnapshot {
    WindowSurfaceIdentity surface_identity{};
    Extent2D              drawable_extent{};
    uint64_t              drawable_generation = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return surface_identity.IsValid() &&
               drawable_extent.x != 0 &&
               drawable_extent.y != 0 &&
               drawable_generation != 0;
    }
};

struct UiViewportDrawPacket {
    static constexpr uint64_t InvalidRecordingSlot =
        std::numeric_limits<uint64_t>::max();

    float2 display_position{};
    float2 display_size{};
    float2 framebuffer_scale{1.f, 1.f};
    uint64_t recording_slot = InvalidRecordingSlot;

    Array<UiDrawVertex>  vertices;
    Array<UiDrawIndex>   indices;
    Array<UiDrawCommand> commands;

    SharedPtr<UiViewportRenderResources> render_resources;
    TextureRef                           framebuffer;
    SwapchainRef                         swapchain;
    WindowFrameSnapshot                  window_frame;
    UiViewportPresentationSnapshot       presentation;
    bool                                 presentation_metadata_only = false;
};

struct UiDrawFramePacket {
    UiDrawFramePacket()                                        = default;
    UiDrawFramePacket(UiDrawFramePacket&&) noexcept            = default;
    UiDrawFramePacket& operator=(UiDrawFramePacket&&) noexcept = default;
    UiDrawFramePacket(const UiDrawFramePacket&)                = delete;
    UiDrawFramePacket& operator=(const UiDrawFramePacket&)     = delete;

    // 数据包需要持有后端，确保渲染线程消费复制后的绘制数据时后端仍然有效。
    SharedPtr<UiDrawFrameBackend> backend;
    UiViewportDrawPacket          main_viewport;
    Array<UiViewportDrawPacket>   platform_viewports;
};

static_assert(std::is_move_constructible_v<UiDrawFramePacket>);
static_assert(!std::is_copy_constructible_v<UiDrawFramePacket>);

// Converts editor/ImGui logical coordinates to the drawable pixel domain used
// by the selected main or detached presentation target.
[[nodiscard]] RENDER_API bool ResolveUiCompositionDrawableMetrics(
    UiCompositionFrameData&      composition,
    const WindowFrameSnapshot&   main_window_frame,
    const UiDrawFramePacket&     draw_frame
) noexcept;
RENDER_API void BindUiViewportWindowFrame(
    UiViewportDrawPacket&      viewport,
    const WindowFrameSnapshot& window_frame
) noexcept;
RENDER_API void BindUiViewportPresentation(
    UiViewportDrawPacket&                 viewport,
    const UiViewportPresentationSnapshot& presentation
) noexcept;
// Finalizes the main-window presentation scale and retargets composition
// metrics from the captured raw drawable domain to the RT-committed extent.
[[nodiscard]] RENDER_API bool RetargetMainUiPresentation(
    UiViewportDrawPacket&      main_viewport,
    UiCompositionFrameData&    composition,
    const WindowFrameSnapshot& window_frame,
    Extent2D                   committed_extent
) noexcept;
[[nodiscard]] RENDER_API bool ConvertUiClipRectToDrawable(
    UiClipRect&   clip_rect,
    const float2& display_position,
    const float2& framebuffer_scale
) noexcept;
[[nodiscard]] RENDER_API bool
IsUiViewportPresentationCommitted(const UiViewportDrawPacket& viewport) noexcept;
[[nodiscard]] RENDER_API bool
IsUiViewportPresentationCurrent(const UiViewportDrawPacket& viewport) noexcept;

/**
 * Freezes the speculative upload-ring slots used by one copied UI frame.
 *
 * Recording alone does not retire a slot. The owner must publish exactly one
 * terminal result after the command source crosses (or fails to cross) the
 * native queue boundary. Duplicate viewport references to the same backend
 * resource share one slot and are committed once.
 */
class RENDER_API UiDrawFrameSlotClaim final {
public:
    enum class EState : uint8_t {
        Pending,
        Accepted,
        Rejected,
    };

    explicit UiDrawFrameSlotClaim(UiDrawFramePacket& frame);

    UiDrawFrameSlotClaim(const UiDrawFrameSlotClaim&)            = delete;
    UiDrawFrameSlotClaim& operator=(const UiDrawFrameSlotClaim&) = delete;
    UiDrawFrameSlotClaim(UiDrawFrameSlotClaim&&)                 = delete;
    UiDrawFrameSlotClaim& operator=(UiDrawFrameSlotClaim&&)      = delete;

    [[nodiscard]] bool IsReadyForRecording() const noexcept;
    [[nodiscard]] bool CommitAccepted() const noexcept;
    void Reject() const noexcept;

    [[nodiscard]] EState GetState() const noexcept {
        return state.load(std::memory_order_acquire);
    }

private:
    struct Slot {
        SharedPtr<UiViewportRenderResources> resources;
        uint64_t                             value = 0;
    };

    void Freeze(UiViewportDrawPacket& viewport);
    [[nodiscard]] bool ValidatePendingSlots() const noexcept;

    Array<Slot>        slots{};
    bool               structurally_valid = true;
    mutable std::atomic<EState> state{EState::Pending};
};

class RENDER_API UiDrawFrameBackend {
public:
    virtual ~UiDrawFrameBackend() = default;

    virtual void RenderGUI(
        CommandList&           _cmd_list,
        const TextureView&     _main_framebuffer,
        const UiDrawFramePacket& _frame,
        EUiDrawExecutionThread _execution_thread
    ) = 0;
    virtual void
    PresentWindows(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread) = 0;
};

RENDER_API void RenderUiDrawFrame(
    CommandList&           _cmd_list,
    const TextureView&     _main_framebuffer,
    const UiDrawFramePacket& _frame,
    EUiDrawExecutionThread _execution_thread
);
RENDER_API void PresentUiDrawFrame(const UiDrawFramePacket& _frame, EUiDrawExecutionThread _execution_thread);

class UIRenderer {
public:
    struct Impl;
    RENDER_API UIRenderer(RenderDevice& _device);

    RENDER_API virtual ~UIRenderer();

    RENDER_API void BeginGUIFrame();

    RENDER_API void EndGUIFrame();

    RENDER_API void UpdatePlatformWindows();

    [[nodiscard]] RENDER_API const WindowInputSourceSnapshot&
    GetInputSnapshot() const noexcept;

    RENDER_API UiDrawFramePacket CaptureDrawFrame();
    RENDER_API void              RegisterImage(Texture* _texture, Sampler _sampler);
    RENDER_API void              UnregisterImage(Texture* _texture);
    [[deprecated("Use UnregisterImage instead")]] RENDER_API void UnRegisterImage(Texture* _texture);

    RENDER_API TextureRef GetWindowFrameBuffer(void* _window);

private:
    UniquePtr<Impl> impl;
};
}; // namespace Moer::Render

#endif // MOER_ENGINE_UI_RENDERER_H
