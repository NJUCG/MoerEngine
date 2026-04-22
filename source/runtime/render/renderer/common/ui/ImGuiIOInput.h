#pragma once

#include "RenderAPI.h"
#include "misc/Traits.h"
#include "window/WindowInput.h"

namespace Moer::Render {

struct ImGuiIOInputSnapshot {
    bool want_capture_mouse    = false;
    bool want_capture_keyboard = false;
    bool want_text_input       = false;

    bool f5_pressed = false;
    bool f8_pressed = false;
    bool reset_speed_down = false;

    float2 mouse_pos{};
    float2 mouse_delta{};
    float  mouse_wheel = 0.0f;

    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_down = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount>     key_down          = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount>     key_released      = {false};
};

struct ImGuiIOInputApplyParams {
    bool scene_active            = false;
    bool play_capture            = false;
    bool external_key_block      = false;
    bool external_cursor_visible = false;
};

RENDER_API ImGuiIOInputSnapshot CaptureImGuiIOInput();
RENDER_API void ApplyImGuiIOInputToWindowInput(
    const ImGuiIOInputSnapshot& snapshot,
    const ImGuiIOInputApplyParams& params
);

} // namespace Moer::Render
