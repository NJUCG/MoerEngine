#include "window/WindowInput.h"

namespace Moer {
namespace {

template<typename ArrayT>
void Clear(ArrayT& values) {
    values.fill(false);
}

bool IsValidKey(KeyButtons key) {
    const auto index = static_cast<uint32_t>(key);
    return index < static_cast<uint32_t>(KeyButtons::KeyButtonCount);
}

bool AnyMouseButtonDown(
    const StaticArray<bool, MouseButtons::MouseButtonCount>& mouse_button_down
) {
    for (bool down : mouse_button_down) {
        if (down) {
            return true;
        }
    }
    return false;
}

WindowInputFrameSnapshot WithoutTransientActions(
    const WindowInputFrameSnapshot& snapshot
) {
    WindowInputFrameSnapshot quiescent = snapshot;
    quiescent.delta_time               = 0.0f;
    quiescent.cursor_delta             = {};
    quiescent.mouse_wheel              = 0.0f;
    quiescent.cursor_mode_changed       = false;
    Clear(quiescent.mouse_button_pressed);
    Clear(quiescent.mouse_button_released);
    Clear(quiescent.key_pressed);
    Clear(quiescent.key_released);
    quiescent.reset_speed = false;
    return quiescent;
}

} // namespace

bool WindowInputFrameTracker::BeginFrame(const WindowInputSourceSnapshot& source) {
    if (m_has_pending_source || source.capture_sequence == 0 ||
        source.capture_sequence <= m_last_capture_sequence) {
        return false;
    }

    m_last_capture_sequence = source.capture_sequence;
    m_pending_source        = source;
    m_has_pending_source    = true;

    if (!source.focused) {
        Clear(m_key_toggle);
        m_cursor_hidden         = false;
        m_cursor_requires_reset = true;
        return true;
    }

    // Toggle edges are reduced before UI policy is built so the scene panel can
    // use F (and future toggles) to decide scene_active in the same GT frame.
    // ImGui keyboard/text capture owns those edges and therefore blocks them.
    if (!source.want_capture_keyboard && !source.want_text_input) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(KeyButtons::KeyButtonCount); ++i) {
            if (source.key_released[i]) {
                m_key_toggle[i] = !m_key_toggle[i];
            }
        }
    }

    return true;
}

bool WindowInputFrameTracker::IsKeyToggled(KeyButtons key) const {
    return IsValidKey(key) ? m_key_toggle[static_cast<uint32_t>(key)] : false;
}

WindowInputFrameSnapshot WindowInputFrameTracker::Finalize(const WindowInputPolicy& policy) {
    if (!m_has_pending_source) {
        return WithoutTransientActions(m_last_snapshot);
    }

    const WindowInputSourceSnapshot source = m_pending_source;
    m_has_pending_source                   = false;

    WindowInputFrameSnapshot frame{};
    frame.capture_sequence    = source.capture_sequence;
    frame.delta_time          = source.gt_delta_time > 0.0f ? source.gt_delta_time : 0.0f;
    frame.focused             = source.focused;
    frame.display_resolution  = source.display_resolution;
    frame.viewport_resolution = policy.viewport_resolution;
    frame.viewport_position   = policy.viewport_position;
    frame.scene_active        = source.focused && policy.scene_active;
    frame.viewport_hovered    = source.focused && policy.viewport_hovered;

    if (source.display_resolution.y != 0u) {
        frame.display_aspect_ratio =
            static_cast<float>(source.display_resolution.x) /
            static_cast<float>(source.display_resolution.y);
    }

    if (!source.focused) {
        // Losing focus is a hard ownership boundary: no held or toggle state,
        // cursor capture, edge, wheel, or camera input may leak to the next
        // consumer. The next cursor-capture frame also starts with zero delta.
        Clear(m_key_toggle);
        frame.key_toggle          = m_key_toggle;
        frame.cursor_mode_changed = m_last_snapshot.cursor_hidden;
        m_cursor_hidden           = false;
        m_cursor_requires_reset   = true;
        m_last_snapshot           = frame;
        return frame;
    }

    // The Scene/Game panel itself contributes WantCaptureMouse while hovered.
    // Once UI policy declares that panel active, its mouse input belongs to the
    // scene even though ImGui still reports capture. Outside the active scene,
    // mouse actions are naturally gated by scene_active.
    frame.mouse_input_allowed = frame.scene_active || frame.viewport_hovered;
    frame.keyboard_input_allowed =
        frame.scene_active && !source.want_capture_keyboard && !source.want_text_input;

    if (frame.mouse_input_allowed) {
        frame.mouse_button_down     = source.mouse_button_down;
        frame.mouse_button_pressed  = source.mouse_button_pressed;
        frame.mouse_button_released = source.mouse_button_released;
        frame.cursor_position       = source.mouse_position;
        frame.mouse_wheel           = source.mouse_wheel;
    } else {
        // Position is metadata rather than an action and remains useful for UI
        // diagnostics even when ImGui owns mouse actions.
        frame.cursor_position = source.mouse_position;
    }

    if (frame.keyboard_input_allowed) {
        frame.key_down     = source.key_down;
        frame.key_pressed  = source.key_pressed;
        frame.key_released = source.key_released;
    }
    frame.key_toggle = m_key_toggle;

    bool cursor_hidden =
        frame.scene_active &&
        (policy.force_cursor_hidden || AnyMouseButtonDown(frame.mouse_button_down) ||
         frame.key_toggle[KeyButtons::F]);
    if (policy.force_cursor_visible) {
        cursor_hidden = false;
    }

    frame.cursor_hidden       = cursor_hidden;
    frame.cursor_mode_changed = cursor_hidden != m_cursor_hidden;
    if (frame.mouse_input_allowed && cursor_hidden && !frame.cursor_mode_changed &&
        !m_cursor_requires_reset) {
        frame.cursor_delta = source.mouse_delta;
    }

    if (frame.cursor_mode_changed || !cursor_hidden) {
        frame.cursor_delta = {};
    }

    m_cursor_hidden = cursor_hidden;
    if (cursor_hidden) {
        m_cursor_requires_reset = false;
    } else {
        m_cursor_requires_reset = true;
    }

    if (frame.keyboard_input_allowed) {
        frame.camera_forward  = frame.key_down[KeyButtons::W];
        frame.camera_backward = frame.key_down[KeyButtons::S];
        frame.camera_left     = frame.key_down[KeyButtons::A];
        frame.camera_right    = frame.key_down[KeyButtons::D];
        frame.camera_up       = frame.key_down[KeyButtons::E];
        frame.camera_down     = frame.key_down[KeyButtons::Q];
        frame.speed_up        = frame.key_down[KeyButtons::UP];
        frame.speed_down      = frame.key_down[KeyButtons::DOWN];
        frame.reset_speed     = source.reset_speed_down;
    }

    m_last_snapshot = frame;
    return frame;
}

} // namespace Moer
