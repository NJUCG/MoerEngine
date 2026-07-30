#ifndef MOERENGINE_WINDOW_INPUT_H
#define MOERENGINE_WINDOW_INPUT_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/Traits.h"

#include <cstdint>

namespace Moer {

// Keep these enums index-compatible with the legacy WindowInput arrays. Native
// backends, ImGui adapters, and camera policy code can therefore migrate to
// snapshots without introducing a second key namespace.
typedef enum {
    Left,
    Middle,
    Right,

    MouseButtonCount,
    MouseButtonFirst = Left,
} MouseButtons;

typedef enum {
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    UP,
    DOWN,
    LEFT,
    RIGHT,
    ESCAPE,

    KeyButtonCount,
    KeyButtonFirst = A,
} KeyButtons;

// Raw, per-poll input captured on the game thread. Edge arrays and wheel/delta
// values describe only capture_sequence; they must not be accumulated across
// snapshots by the producer.
struct RENDER_API WindowInputSourceSnapshot {
    uint64_t capture_sequence = 0;
    float    gt_delta_time    = 0.0f;
    bool     focused          = true;
    uint32_t focused_viewport_id = 0;

    uint2  display_resolution = uint2(1280u, 720u);
    float2 mouse_position{};
    float2 mouse_delta{};
    float  mouse_wheel = 0.0f;

    bool want_capture_mouse    = false;
    bool want_capture_keyboard = false;
    bool want_text_input       = false;

    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_down     = {false};
    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_pressed  = {false};
    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_released = {false};

    StaticArray<bool, KeyButtons::KeyButtonCount> key_down     = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount> key_pressed  = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount> key_released = {false};

    // The legacy camera reset chord is not represented by KeyButtons. The
    // platform/ImGui adapter resolves that chord before publishing a source.
    bool reset_speed_down = false;
};

// UI-owned policy resolved after BeginFrame. Viewport geometry remains logical
// editor-space geometry; display_resolution above describes the host window.
struct RENDER_API WindowInputPolicy {
    bool scene_active         = false;
    bool viewport_hovered     = false;
    bool force_cursor_hidden  = false;
    bool force_cursor_visible = false;

    uint2 viewport_resolution = uint2(0u, 0u);
    uint2 viewport_position   = uint2(0u, 0u);
};

// Free-look gets its own ownership latch instead of reusing generic cursor
// capture, which may have been established by an ordinary mouse drag. It can
// start only over the active Scene/Game viewport, then survives GLFW's
// disabled-cursor virtual position moving outside the content rectangle.
[[nodiscard]] constexpr uint32_t ResolveFreeLookCaptureViewportId(
    uint32_t focused_viewport_id,
    bool     free_look_toggled,
    bool     viewport_hovered,
    uint32_t active_viewport_id,
    uint32_t previous_free_look_viewport_id
) noexcept {
    if (!free_look_toggled || active_viewport_id == 0 ||
        focused_viewport_id != active_viewport_id) {
        return 0;
    }
    if (previous_free_look_viewport_id == active_viewport_id) {
        return active_viewport_id;
    }
    return viewport_hovered ? active_viewport_id : 0;
}

// Mouse-drag ownership is also tied to the platform viewport where the click
// began. An existing owner cannot move merely because focus/active ownership
// changed; a newly focused viewport must receive its own click edge.
[[nodiscard]] constexpr uint32_t ResolveMouseCaptureViewportId(
    uint32_t focused_viewport_id,
    uint32_t active_viewport_id,
    bool     viewport_hovered,
    bool     mouse_clicked,
    bool     has_stale_mouse_button_down,
    uint32_t previous_mouse_capture_viewport_id
) noexcept {
    if (previous_mouse_capture_viewport_id != 0 &&
        previous_mouse_capture_viewport_id == focused_viewport_id &&
        previous_mouse_capture_viewport_id == active_viewport_id) {
        return previous_mouse_capture_viewport_id;
    }
    return mouse_clicked && !has_stale_mouse_button_down && viewport_hovered &&
                   active_viewport_id != 0 &&
                   focused_viewport_id == active_viewport_id
               ? active_viewport_id
               : 0;
}

// Immutable-by-ownership frame value: it contains no references to tracker,
// source, or policy storage. Copies can be passed to camera/UI consumers and
// across task boundaries without observing later BeginFrame calls.
struct RENDER_API WindowInputFrameSnapshot {
    uint64_t capture_sequence = 0;
    float    delta_time       = 0.0f;
    bool     focused          = false;
    uint32_t focused_viewport_id = 0;

    uint2 display_resolution  = uint2(0u, 0u);
    float display_aspect_ratio = 0.0f;

    uint2 viewport_resolution = uint2(0u, 0u);
    uint2 viewport_position   = uint2(0u, 0u);
    bool  scene_active        = false;
    bool  viewport_hovered    = false;

    float2 cursor_position{};
    float2 cursor_delta{};
    float  mouse_wheel = 0.0f;

    bool mouse_input_allowed    = false;
    bool keyboard_input_allowed = false;
    bool cursor_hidden           = false;
    bool cursor_mode_changed     = false;

    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_down     = {false};
    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_pressed  = {false};
    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_released = {false};

    StaticArray<bool, KeyButtons::KeyButtonCount> key_down     = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount> key_pressed  = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount> key_released = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount> key_toggle   = {false};

    bool camera_forward  = false;
    bool camera_backward = false;
    bool camera_left     = false;
    bool camera_right    = false;
    bool camera_up       = false;
    bool camera_down     = false;
    bool speed_up        = false;
    bool speed_down      = false;
    bool reset_speed     = false;
};

// Deterministic, CPU-only reducer. BeginFrame accepts each source sequence at
// most once and exposes it for UI policy construction. Finalize consumes that
// pending source exactly once and returns a self-contained frame value.
class RENDER_API WindowInputFrameTracker {
public:
    bool BeginFrame(const WindowInputSourceSnapshot& source);

    [[nodiscard]] bool HasPendingSource() const {
        return m_has_pending_source;
    }

    [[nodiscard]] const WindowInputSourceSnapshot& GetPendingSource() const {
        return m_pending_source;
    }

    [[nodiscard]] bool IsKeyToggled(KeyButtons key) const;
    void ClearKeyToggle(KeyButtons key) noexcept;

    [[nodiscard]] WindowInputFrameSnapshot Finalize(const WindowInputPolicy& policy);

    [[nodiscard]] const WindowInputFrameSnapshot& GetLatest() const {
        return m_last_snapshot;
    }

private:
    uint64_t m_last_capture_sequence = 0;
    bool     m_has_pending_source    = false;
    bool     m_cursor_hidden         = false;
    bool     m_cursor_requires_reset = true;

    WindowInputSourceSnapshot m_pending_source{};
    WindowInputFrameSnapshot  m_last_snapshot{};
    StaticArray<bool, KeyButtons::KeyButtonCount> m_key_toggle = {false};
};

} // namespace Moer

#endif
