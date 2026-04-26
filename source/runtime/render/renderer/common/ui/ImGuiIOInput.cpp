#include "ImGuiIOInput.h"

#include <imgui.h>

namespace Moer::Render {
namespace {

ImGuiKey ToImGuiKey(KeyButtons key) {
    switch (key) {
        case KeyButtons::A:
            return ImGuiKey_A;
        case KeyButtons::B:
            return ImGuiKey_B;
        case KeyButtons::C:
            return ImGuiKey_C;
        case KeyButtons::D:
            return ImGuiKey_D;
        case KeyButtons::E:
            return ImGuiKey_E;
        case KeyButtons::F:
            return ImGuiKey_F;
        case KeyButtons::G:
            return ImGuiKey_G;
        case KeyButtons::H:
            return ImGuiKey_H;
        case KeyButtons::I:
            return ImGuiKey_I;
        case KeyButtons::J:
            return ImGuiKey_J;
        case KeyButtons::K:
            return ImGuiKey_K;
        case KeyButtons::L:
            return ImGuiKey_L;
        case KeyButtons::M:
            return ImGuiKey_M;
        case KeyButtons::N:
            return ImGuiKey_N;
        case KeyButtons::O:
            return ImGuiKey_O;
        case KeyButtons::P:
            return ImGuiKey_P;
        case KeyButtons::Q:
            return ImGuiKey_Q;
        case KeyButtons::R:
            return ImGuiKey_R;
        case KeyButtons::S:
            return ImGuiKey_S;
        case KeyButtons::T:
            return ImGuiKey_T;
        case KeyButtons::U:
            return ImGuiKey_U;
        case KeyButtons::V:
            return ImGuiKey_V;
        case KeyButtons::W:
            return ImGuiKey_W;
        case KeyButtons::X:
            return ImGuiKey_X;
        case KeyButtons::Y:
            return ImGuiKey_Y;
        case KeyButtons::Z:
            return ImGuiKey_Z;
        case KeyButtons::UP:
            return ImGuiKey_UpArrow;
        case KeyButtons::DOWN:
            return ImGuiKey_DownArrow;
        case KeyButtons::LEFT:
            return ImGuiKey_LeftArrow;
        case KeyButtons::RIGHT:
            return ImGuiKey_RightArrow;
        case KeyButtons::ESCAPE:
            return ImGuiKey_Escape;
        case KeyButtons::GRAVE_ACCENT:
            return ImGuiKey_GraveAccent;
        case KeyButtons::F5:
            return ImGuiKey_F5;
        case KeyButtons::F8:
            return ImGuiKey_F8;
        default:
            return ImGuiKey_None;
    }
}

void ClearCameraInput(WindowInput& input) {
    input.camera_forward  = false;
    input.camera_backward = false;
    input.camera_left     = false;
    input.camera_right    = false;
    input.camera_up       = false;
    input.camera_down     = false;
    input.speed_up        = false;
    input.speed_down      = false;
    input.reset_speed     = false;
}

} // namespace

ImGuiIOInputSnapshot CaptureImGuiIOInput() {
    ImGuiIOInputSnapshot snapshot{};
    ImGuiIO&             io = ImGui::GetIO();

    snapshot.want_capture_mouse    = io.WantCaptureMouse;
    snapshot.want_capture_keyboard = io.WantCaptureKeyboard;
    snapshot.want_text_input       = io.WantTextInput;
    snapshot.f5_pressed            = ImGui::IsKeyPressed(ImGuiKey_F5, false);
    snapshot.f8_pressed            = ImGui::IsKeyPressed(ImGuiKey_F8, false);
    snapshot.reset_speed_down      = io.KeyCtrl && ImGui::IsKeyDown(ImGuiKey_Keypad0);
    snapshot.mouse_pos             = {io.MousePos.x, io.MousePos.y};
    snapshot.mouse_delta           = {io.MouseDelta.x, io.MouseDelta.y};
    snapshot.mouse_wheel           = io.MouseWheel;

    snapshot.mouse_button_down[MouseButtons::Left]   = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    snapshot.mouse_button_down[MouseButtons::Middle] = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    snapshot.mouse_button_down[MouseButtons::Right]  = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    for (uint32_t i = 0; i < KeyButtons::KeyButtonCount; ++i) {
        const ImGuiKey imgui_key = ToImGuiKey(static_cast<KeyButtons>(i));
        if (imgui_key == ImGuiKey_None) {
            continue;
        }
        snapshot.key_down[i]     = ImGui::IsKeyDown(imgui_key);
        snapshot.key_released[i] = ImGui::IsKeyReleased(imgui_key);
    }

    return snapshot;
}

void ApplyImGuiIOInputToWindowInput(
    const ImGuiIOInputSnapshot& snapshot,
    const ImGuiIOInputApplyParams& params
) {
    WindowInput& input = WindowInput::Get();

    const bool play_capture    = params.play_capture;
    const bool scene_active    = play_capture || params.scene_active;
    const bool keyboard_block  = params.external_key_block ||
                                (!play_capture && (snapshot.want_capture_keyboard || snapshot.want_text_input));
    const bool mouse_block     = !play_capture && !scene_active && snapshot.want_capture_mouse;
    const bool allow_keyboard  = scene_active && !keyboard_block;
    const bool allow_mouse     = scene_active && !mouse_block;

    input.is_active                = scene_active;
    input.force_cursor_hidden      = play_capture;
    input.force_cursor_visible     = play_capture ? false : params.external_cursor_visible;
    input.play_mode_camera_control = play_capture;
    input.block_camera_keyboard_input = keyboard_block;

    input.cursor_last_x = snapshot.mouse_pos.x;
    input.cursor_last_y = snapshot.mouse_pos.y;
    if (input.is_cursor_dirty) {
        input.cursor_delta_x = 0.0f;
        input.cursor_delta_y = 0.0f;
        input.is_cursor_dirty = false;
    } else if (allow_mouse) {
        input.cursor_delta_x = snapshot.mouse_delta.x;
        input.cursor_delta_y = snapshot.mouse_delta.y;
    } else {
        input.cursor_delta_x = 0.0f;
        input.cursor_delta_y = 0.0f;
    }

    input.scroll_offset = allow_mouse ? snapshot.mouse_wheel : 0.0f;

    for (uint32_t i = 0; i < MouseButtons::MouseButtonCount; ++i) {
        input.mouse_button_state[i] = allow_mouse && snapshot.mouse_button_down[i];
    }

    for (uint32_t i = 0; i < KeyButtons::KeyButtonCount; ++i) {
        input.key_button_state[i] = allow_keyboard && snapshot.key_down[i];
        if (allow_keyboard && snapshot.key_released[i]) {
            input.key_button_switch_state[i] = !input.key_button_switch_state[i];
        }
    }

    ClearCameraInput(input);
    if (!allow_keyboard) {
        return;
    }

    input.camera_forward  = snapshot.key_down[KeyButtons::W];
    input.camera_backward = snapshot.key_down[KeyButtons::S];
    input.camera_left     = snapshot.key_down[KeyButtons::A];
    input.camera_right    = snapshot.key_down[KeyButtons::D];
    input.camera_up       = snapshot.key_down[KeyButtons::E];
    input.camera_down     = snapshot.key_down[KeyButtons::Q];
    input.speed_up        = snapshot.key_down[KeyButtons::UP];
    input.speed_down      = snapshot.key_down[KeyButtons::DOWN];
    input.reset_speed     = snapshot.reset_speed_down;
}

} // namespace Moer::Render
