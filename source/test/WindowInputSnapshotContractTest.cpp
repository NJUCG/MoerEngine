#include "renderer/EditorConfig.h"
#include "scene/camera/Camera.h"
#include "window/WindowInput.h"

#include <cstdlib>

using namespace Moer;

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void RequireUint2(const uint2& actual, uint32_t x, uint32_t y) {
    Require(actual.x == x);
    Require(actual.y == y);
}

void RequireFloat2(const float2& actual, float x, float y) {
    Require(actual.x == x);
    Require(actual.y == y);
}

WindowInputSourceSnapshot MakeSource(uint64_t sequence) {
    WindowInputSourceSnapshot source{};
    source.capture_sequence  = sequence;
    source.gt_delta_time     = 1.0f / 60.0f;
    source.focused           = true;
    source.focused_viewport_id = 17u;
    source.display_resolution = uint2(1920u, 1080u);
    source.mouse_position    = float2(320.0f, 180.0f);
    return source;
}

WindowInputPolicy MakeActivePolicy() {
    return {
        .scene_active        = true,
        .viewport_hovered    = true,
        .viewport_resolution = uint2(1280u, 720u),
        .viewport_position   = uint2(40u, 60u),
    };
}

} // namespace

int main() {
    // GLFW disabled-cursor mode reports an unbounded virtual position. Once F
    // has committed its own viewport owner, losing geometric hover must not end
    // free-look. The latch is deliberately independent from mouse-drag cursor
    // ownership, so a latent F toggle cannot adopt an unrelated capture ID.
    Require(ResolveFreeLookCaptureViewportId(17u, true, true, 17u, 0u) == 17u);
    Require(ResolveFreeLookCaptureViewportId(17u, true, false, 17u, 17u) == 17u);
    Require(ResolveFreeLookCaptureViewportId(17u, true, false, 17u, 0u) == 0u);
    Require(ResolveFreeLookCaptureViewportId(17u, false, false, 17u, 17u) == 0u);
    Require(ResolveFreeLookCaptureViewportId(0u, true, false, 17u, 17u) == 0u);
    Require(ResolveFreeLookCaptureViewportId(23u, true, false, 17u, 17u) == 0u);
    Require(ResolveFreeLookCaptureViewportId(23u, true, false, 23u, 17u) == 0u);
    Require(ResolveFreeLookCaptureViewportId(23u, true, true, 23u, 17u) == 23u);
    Require(ResolveFreeLookCaptureViewportId(17u, true, false, 0u, 17u) == 0u);

    // Mouse-drag capture is owned by the platform viewport where its click
    // began. It survives geometric hover loss inside that viewport, but cannot
    // transfer merely because focus/active ownership moves to another detached
    // window. A fresh click in that new window establishes a new owner.
    Require(ResolveMouseCaptureViewportId(17u, 17u, true, true, false, 0u) == 17u);
    Require(ResolveMouseCaptureViewportId(17u, 17u, false, false, true, 17u) == 17u);
    Require(ResolveMouseCaptureViewportId(23u, 23u, true, false, false, 17u) == 0u);
    Require(ResolveMouseCaptureViewportId(23u, 23u, true, true, false, 17u) == 23u);
    Require(ResolveMouseCaptureViewportId(23u, 23u, true, true, true, 17u) == 0u);
    Require(ResolveMouseCaptureViewportId(23u, 23u, true, true, false, 0u) == 23u);
    Require(ResolveMouseCaptureViewportId(17u, 23u, true, true, false, 0u) == 0u);
    Require(ResolveMouseCaptureViewportId(0u, 17u, true, true, false, 17u) == 0u);
    Require(ResolveMouseCaptureViewportId(17u, 0u, true, true, false, 17u) == 0u);
    Require(ResolveMouseCaptureViewportId(17u, 17u, false, true, false, 0u) == 0u);

    // Engine-only callers have no UI hook. An empty input value must preserve
    // CameraFrameInput's safe projection default instead of publishing aspect 0.
    const CameraFrameInput engine_only_input =
        CameraFrameInput::Capture(WindowInputFrameSnapshot{}, EditorConfig{});
    Require(engine_only_input.window_aspect_ratio == 16.0f / 9.0f);

    WindowInputFrameTracker tracker;
    const WindowInputPolicy active_policy = MakeActivePolicy();

    // UI ownership may revoke a persistent toggle after BeginFrame when the
    // captured platform viewport loses focus or is replaced.
    WindowInputFrameTracker clear_toggle_tracker;
    WindowInputSourceSnapshot clear_toggle_source = MakeSource(1);
    clear_toggle_source.key_released[KeyButtons::F] = true;
    Require(clear_toggle_tracker.BeginFrame(clear_toggle_source));
    Require(clear_toggle_tracker.IsKeyToggled(KeyButtons::F));
    clear_toggle_tracker.ClearKeyToggle(KeyButtons::F);
    Require(!clear_toggle_tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot clear_toggle_frame =
        clear_toggle_tracker.Finalize(active_policy);
    Require(!clear_toggle_frame.key_toggle[KeyButtons::F]);
    Require(!clear_toggle_frame.cursor_hidden);

    // A text field in a docked Editor panel shares platform focus with the
    // active Scene/Game viewport. The UI integration revokes its owners,
    // clears free-look, suppresses viewport input, and forces a visible cursor.
    WindowInputFrameTracker   text_input_tracker;
    WindowInputSourceSnapshot text_input_free_look   = MakeSource(1);
    text_input_free_look.key_released[KeyButtons::F] = true;
    Require(text_input_tracker.BeginFrame(text_input_free_look));
    const WindowInputFrameSnapshot text_input_captured = text_input_tracker.Finalize(active_policy);
    Require(text_input_captured.cursor_hidden);
    Require(text_input_captured.key_toggle[KeyButtons::F]);

    WindowInputSourceSnapshot text_input_focus = MakeSource(2);
    text_input_focus.want_capture_keyboard     = true;
    text_input_focus.want_text_input           = true;
    text_input_focus.mouse_delta               = float2(8.0f, -4.0f);
    Require(text_input_tracker.BeginFrame(text_input_focus));
    text_input_tracker.ClearKeyToggle(KeyButtons::F);
    WindowInputPolicy text_input_policy             = active_policy;
    text_input_policy.scene_active                  = false;
    text_input_policy.viewport_hovered              = false;
    text_input_policy.force_cursor_visible          = true;
    const WindowInputFrameSnapshot text_input_frame = text_input_tracker.Finalize(text_input_policy);
    Require(!text_input_frame.scene_active);
    Require(!text_input_frame.mouse_input_allowed);
    Require(!text_input_frame.keyboard_input_allowed);
    Require(!text_input_frame.key_toggle[KeyButtons::F]);
    Require(!text_input_frame.cursor_hidden);
    Require(text_input_frame.cursor_mode_changed);
    RequireFloat2(text_input_frame.cursor_delta, 0.0f, 0.0f);

    WindowInputSourceSnapshot text_input_released = MakeSource(3);
    Require(text_input_tracker.BeginFrame(text_input_released));
    Require(!text_input_tracker.IsKeyToggled(KeyButtons::F));
    const uint32_t resumed_free_look_owner = ResolveFreeLookCaptureViewportId(
        text_input_released.focused_viewport_id, text_input_tracker.IsKeyToggled(KeyButtons::F), true, 17u, 0u
    );
    Require(resumed_free_look_owner == 0u);
    WindowInputPolicy text_input_released_policy = active_policy;
    text_input_released_policy.scene_active      = false;
    const WindowInputFrameSnapshot text_input_released_frame =
        text_input_tracker.Finalize(text_input_released_policy);
    Require(!text_input_released_frame.cursor_hidden);
    Require(!text_input_released_frame.key_toggle[KeyButtons::F]);

    // Releasing F away from active viewport content must not leave a latent
    // toggle that starts free-look on a later hover without another F edge.
    WindowInputFrameTracker latent_toggle_tracker;
    WindowInputSourceSnapshot latent_toggle_source = MakeSource(1);
    latent_toggle_source.key_released[KeyButtons::F] = true;
    Require(latent_toggle_tracker.BeginFrame(latent_toggle_source));
    const uint32_t rejected_free_look_owner = ResolveFreeLookCaptureViewportId(
        latent_toggle_source.focused_viewport_id,
        latent_toggle_tracker.IsKeyToggled(KeyButtons::F),
        false,
        17u,
        0u
    );
    Require(rejected_free_look_owner == 0u);
    if (latent_toggle_tracker.IsKeyToggled(KeyButtons::F) &&
        rejected_free_look_owner == 0u) {
        latent_toggle_tracker.ClearKeyToggle(KeyButtons::F);
    }
    Require(!latent_toggle_tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot rejected_free_look_frame =
        latent_toggle_tracker.Finalize(active_policy);
    Require(!rejected_free_look_frame.key_toggle[KeyButtons::F]);
    Require(!rejected_free_look_frame.cursor_hidden);

    WindowInputSourceSnapshot later_hover_source = MakeSource(2);
    Require(latent_toggle_tracker.BeginFrame(later_hover_source));
    Require(
        ResolveFreeLookCaptureViewportId(
            later_hover_source.focused_viewport_id,
            latent_toggle_tracker.IsKeyToggled(KeyButtons::F),
            true,
            17u,
            0u
        ) == 0u
    );
    (void)latent_toggle_tracker.Finalize(active_policy);

    // First physical F release toggles exactly once and is visible to UI before
    // Finalize, allowing policy construction in the same frame.
    WindowInputSourceSnapshot first = MakeSource(1);
    first.key_released[KeyButtons::F] = true;
    first.key_down[KeyButtons::W]     = true;
    Require(tracker.BeginFrame(first));
    Require(tracker.HasPendingSource());
    Require(tracker.GetPendingSource().capture_sequence == 1);
    Require(tracker.IsKeyToggled(KeyButtons::F));

    const WindowInputFrameSnapshot frame1 = tracker.Finalize(active_policy);
    Require(!tracker.HasPendingSource());
    Require(frame1.capture_sequence == 1);
    Require(frame1.delta_time == first.gt_delta_time);
    Require(frame1.focused_viewport_id == 17u);
    RequireUint2(frame1.display_resolution, 1920u, 1080u);
    Require(frame1.display_aspect_ratio > 1.77f && frame1.display_aspect_ratio < 1.78f);
    RequireUint2(frame1.viewport_resolution, 1280u, 720u);
    RequireUint2(frame1.viewport_position, 40u, 60u);
    Require(frame1.scene_active);
    Require(frame1.keyboard_input_allowed);
    Require(frame1.key_down[KeyButtons::W]);
    Require(frame1.key_released[KeyButtons::F]);
    Require(frame1.key_toggle[KeyButtons::F]);
    Require(frame1.camera_forward);
    Require(frame1.cursor_hidden);
    Require(frame1.cursor_mode_changed);
    RequireFloat2(frame1.cursor_delta, 0.0f, 0.0f);

    // A duplicate or stale capture cannot replay its release edge.
    Require(!tracker.BeginFrame(first));
    Require(tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot duplicate_finalize = tracker.Finalize(active_policy);
    Require(duplicate_finalize.capture_sequence == 1);
    Require(duplicate_finalize.key_toggle[KeyButtons::F]);
    Require(duplicate_finalize.delta_time == 0.0f);
    Require(!duplicate_finalize.key_released[KeyButtons::F]);
    Require(!duplicate_finalize.cursor_mode_changed);

    WindowInputSourceSnapshot stale = MakeSource(0);
    stale.key_released[KeyButtons::F] = true;
    Require(!tracker.BeginFrame(stale));
    Require(tracker.IsKeyToggled(KeyButtons::F));

    // Once cursor capture is stable, mouse delta is delivered. Wheel is a
    // per-frame impulse and disappears on the next source.
    WindowInputSourceSnapshot motion = MakeSource(2);
    motion.mouse_delta = float2(7.0f, -3.0f);
    motion.mouse_wheel = 2.0f;
    Require(tracker.BeginFrame(motion));
    WindowInputSourceSnapshot overlapping = MakeSource(3);
    overlapping.key_released[KeyButtons::F] = true;
    Require(!tracker.BeginFrame(overlapping));
    Require(tracker.GetPendingSource().capture_sequence == 2);
    Require(tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot frame2 = tracker.Finalize(active_policy);
    Require(frame2.cursor_hidden);
    Require(!frame2.cursor_mode_changed);
    RequireFloat2(frame2.cursor_delta, 7.0f, -3.0f);
    Require(frame2.mouse_wheel == 2.0f);
    const WindowInputFrameSnapshot frame2_quiescent = tracker.Finalize(active_policy);
    Require(frame2_quiescent.capture_sequence == 2);
    Require(frame2_quiescent.delta_time == 0.0f);
    RequireFloat2(frame2_quiescent.cursor_delta, 0.0f, 0.0f);
    Require(frame2_quiescent.mouse_wheel == 0.0f);
    Require(!frame2_quiescent.cursor_mode_changed);

    WindowInputSourceSnapshot no_wheel = overlapping;
    no_wheel.key_released[KeyButtons::F] = false;
    no_wheel.mouse_delta = float2(1.0f, 2.0f);
    Require(tracker.BeginFrame(no_wheel));
    const WindowInputFrameSnapshot frame3 = tracker.Finalize(active_policy);
    Require(frame3.mouse_wheel == 0.0f);
    RequireFloat2(frame3.cursor_delta, 1.0f, 2.0f);

    // Keyboard capture and text input independently gate held/edge/camera
    // actions and prevent toggle edges from entering persistent state.
    WindowInputSourceSnapshot keyboard_capture = MakeSource(4);
    keyboard_capture.want_capture_keyboard       = true;
    keyboard_capture.key_down[KeyButtons::W]     = true;
    keyboard_capture.key_pressed[KeyButtons::A]  = true;
    keyboard_capture.key_released[KeyButtons::F] = true;
    Require(tracker.BeginFrame(keyboard_capture));
    Require(tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot frame4 = tracker.Finalize(active_policy);
    Require(!frame4.keyboard_input_allowed);
    Require(!frame4.key_down[KeyButtons::W]);
    Require(!frame4.key_pressed[KeyButtons::A]);
    Require(!frame4.key_released[KeyButtons::F]);
    Require(frame4.key_toggle[KeyButtons::F]);
    Require(!frame4.camera_forward);

    WindowInputSourceSnapshot text_capture = MakeSource(5);
    text_capture.want_text_input            = true;
    text_capture.key_down[KeyButtons::D]    = true;
    text_capture.key_released[KeyButtons::F] = true;
    Require(tracker.BeginFrame(text_capture));
    Require(tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot frame5 = tracker.Finalize(active_policy);
    Require(!frame5.keyboard_input_allowed);
    Require(!frame5.camera_right);

    // An active Scene/Game panel owns its mouse even though that ImGui panel
    // contributes WantCaptureMouse.
    WindowInputSourceSnapshot mouse_capture = MakeSource(6);
    mouse_capture.want_capture_mouse = true;
    mouse_capture.mouse_button_down[MouseButtons::Left] = true;
    mouse_capture.mouse_button_pressed[MouseButtons::Left] = true;
    mouse_capture.mouse_delta = float2(5.0f, 5.0f);
    mouse_capture.mouse_wheel = -1.0f;
    Require(tracker.BeginFrame(mouse_capture));
    const WindowInputFrameSnapshot frame6 = tracker.Finalize(active_policy);
    Require(frame6.mouse_input_allowed);
    Require(frame6.mouse_button_down[MouseButtons::Left]);
    Require(frame6.mouse_button_pressed[MouseButtons::Left]);
    Require(frame6.mouse_wheel == -1.0f);
    RequireFloat2(
        frame6.cursor_position,
        mouse_capture.mouse_position.x,
        mouse_capture.mouse_position.y
    );

    // Losing focus is a hard reset for held/toggle/cursor/camera state.
    WindowInputSourceSnapshot focus_lost = MakeSource(7);
    focus_lost.focused                    = false;
    focus_lost.focused_viewport_id        = 0;
    focus_lost.key_down[KeyButtons::W]    = true;
    focus_lost.mouse_button_down[MouseButtons::Left] = true;
    focus_lost.mouse_delta = float2(9.0f, 9.0f);
    focus_lost.mouse_wheel = 4.0f;
    Require(tracker.BeginFrame(focus_lost));
    Require(!tracker.IsKeyToggled(KeyButtons::F));
    const WindowInputFrameSnapshot frame7 = tracker.Finalize(active_policy);
    Require(!frame7.focused);
    Require(!frame7.scene_active);
    Require(!frame7.key_down[KeyButtons::W]);
    Require(!frame7.mouse_button_down[MouseButtons::Left]);
    Require(!frame7.key_toggle[KeyButtons::F]);
    Require(!frame7.cursor_hidden);
    RequireFloat2(frame7.cursor_delta, 0.0f, 0.0f);
    Require(frame7.mouse_wheel == 0.0f);
    Require(!frame7.camera_forward);

    // Regaining focus while entering hidden-cursor mode resets the first delta.
    WindowInputSourceSnapshot refocused = MakeSource(8);
    refocused.mouse_button_down[MouseButtons::Right] = true;
    refocused.mouse_delta = float2(100.0f, 100.0f);
    Require(tracker.BeginFrame(refocused));
    const WindowInputFrameSnapshot frame8 = tracker.Finalize(active_policy);
    Require(frame8.cursor_hidden);
    Require(frame8.cursor_mode_changed);
    RequireFloat2(frame8.cursor_delta, 0.0f, 0.0f);

    WindowInputSourceSnapshot stable_hidden = MakeSource(9);
    stable_hidden.mouse_button_down[MouseButtons::Right] = true;
    stable_hidden.mouse_delta = float2(3.0f, 4.0f);
    Require(tracker.BeginFrame(stable_hidden));
    const WindowInputFrameSnapshot frame9 = tracker.Finalize(active_policy);
    Require(frame9.cursor_hidden);
    Require(!frame9.cursor_mode_changed);
    RequireFloat2(frame9.cursor_delta, 3.0f, 4.0f);

    // Explicit cursor visibility is another mode transition and zeros delta.
    WindowInputPolicy visible_policy = active_policy;
    visible_policy.force_cursor_visible = true;
    WindowInputSourceSnapshot visible = MakeSource(10);
    visible.mouse_button_down[MouseButtons::Right] = true;
    visible.mouse_delta = float2(11.0f, 12.0f);
    Require(tracker.BeginFrame(visible));
    const WindowInputFrameSnapshot frame10 = tracker.Finalize(visible_policy);
    Require(!frame10.cursor_hidden);
    Require(frame10.cursor_mode_changed);
    RequireFloat2(frame10.cursor_delta, 0.0f, 0.0f);

    // Returned snapshots own their data. Later sources and policies cannot
    // mutate an earlier copy.
    const WindowInputFrameSnapshot immutable_copy = frame9;
    WindowInputSourceSnapshot      later          = MakeSource(11);
    later.display_resolution                      = uint2(800u, 600u);
    later.mouse_button_down[MouseButtons::Left]   = true;
    later.mouse_wheel                             = 6.0f;
    later.key_down[KeyButtons::W]                 = true;
    Require(tracker.BeginFrame(later));
    WindowInputPolicy              inactive_policy{};
    const WindowInputFrameSnapshot frame11 = tracker.Finalize(inactive_policy);
    Require(frame11.capture_sequence == 11);
    Require(!frame11.scene_active);
    Require(!frame11.cursor_hidden);
    Require(!frame11.mouse_input_allowed);
    Require(!frame11.keyboard_input_allowed);
    Require(!frame11.mouse_button_down[MouseButtons::Left]);
    Require(!frame11.key_down[KeyButtons::W]);
    Require(frame11.mouse_wheel == 0.0f);
    Require(immutable_copy.capture_sequence == 9);
    RequireUint2(immutable_copy.display_resolution, 1920u, 1080u);
    RequireFloat2(immutable_copy.cursor_delta, 3.0f, 4.0f);

    // Hover owns viewport-local wheel input without forcing cursor capture or
    // keyboard camera control. WantCaptureMouse is expected for an ImGui-hosted
    // viewport and must not discard the viewport's own wheel event.
    WindowInputSourceSnapshot hover_only = MakeSource(12);
    hover_only.want_capture_mouse         = true;
    hover_only.mouse_wheel                = 3.0f;
    Require(tracker.BeginFrame(hover_only));
    WindowInputPolicy hover_policy = active_policy;
    hover_policy.scene_active      = false;
    const WindowInputFrameSnapshot frame12 = tracker.Finalize(hover_policy);
    Require(!frame12.scene_active);
    Require(frame12.viewport_hovered);
    Require(frame12.mouse_input_allowed);
    Require(!frame12.keyboard_input_allowed);
    Require(!frame12.cursor_hidden);
    Require(frame12.mouse_wheel == 3.0f);

    // A drag owns its matching release even after the pointer leaves the
    // viewport. EditorUI keeps scene_active true through this release frame.
    WindowInputSourceSnapshot drag_start = MakeSource(13);
    drag_start.mouse_button_down[MouseButtons::Left]    = true;
    drag_start.mouse_button_pressed[MouseButtons::Left] = true;
    Require(tracker.BeginFrame(drag_start));
    const WindowInputFrameSnapshot frame13 = tracker.Finalize(active_policy);
    Require(frame13.mouse_button_pressed[MouseButtons::Left]);
    Require(frame13.cursor_hidden);

    WindowInputSourceSnapshot drag_release = MakeSource(14);
    drag_release.mouse_button_released[MouseButtons::Left] = true;
    Require(tracker.BeginFrame(drag_release));
    WindowInputPolicy release_policy = active_policy;
    release_policy.viewport_hovered  = false;
    const WindowInputFrameSnapshot frame14 = tracker.Finalize(release_policy);
    Require(frame14.scene_active);
    Require(!frame14.viewport_hovered);
    Require(frame14.mouse_input_allowed);
    Require(frame14.mouse_button_released[MouseButtons::Left]);
    Require(!frame14.cursor_hidden);
    Require(frame14.cursor_mode_changed);

    return 0;
}
