#include "ImGuiIOInput.h"

#include "RenderThread.h"

#include <cassert>
#include <cmath>
#include <imgui.h>

namespace Moer::Render {
namespace {

[[nodiscard]] ImGuiKey ToImGuiKey(KeyButtons key) noexcept {
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
        default:
            return ImGuiKey_None;
    }
}

[[nodiscard]] uint32_t ToDisplayExtent(float value) noexcept {
    if (!std::isfinite(value) || value <= 0.0f) {
        return 0;
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] bool HasFocusedPlatformViewport() {
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (platform_io.Platform_GetWindowFocus == nullptr) {
        return !ImGui::GetIO().AppFocusLost;
    }

    for (ImGuiViewport* viewport : platform_io.Viewports) {
        if (viewport != nullptr && viewport->PlatformHandle != nullptr &&
            platform_io.Platform_GetWindowFocus(viewport)) {
            return true;
        }
    }
    return false;
}

} // namespace

WindowInputSourceSnapshot CaptureImGuiIOInput(uint64_t capture_sequence) {
    assert(!IsRenderThreadInitialized() || IsCurrentlyGameThread());

    WindowInputSourceSnapshot snapshot{};
    const ImGuiIO&            io = ImGui::GetIO();

    snapshot.capture_sequence      = capture_sequence;
    snapshot.gt_delta_time         = io.DeltaTime;
    snapshot.focused               = HasFocusedPlatformViewport();
    snapshot.display_resolution    = uint2(
        ToDisplayExtent(io.DisplaySize.x),
        ToDisplayExtent(io.DisplaySize.y)
    );
    snapshot.mouse_position        = float2(io.MousePos.x, io.MousePos.y);
    snapshot.mouse_delta           = float2(io.MouseDelta.x, io.MouseDelta.y);
    snapshot.mouse_wheel           = io.MouseWheel;
    snapshot.want_capture_mouse    = io.WantCaptureMouse;
    snapshot.want_capture_keyboard = io.WantCaptureKeyboard;
    snapshot.want_text_input       = io.WantTextInput;
    snapshot.reset_speed_down      = io.KeyCtrl && ImGui::IsKeyDown(ImGuiKey_Keypad0);

    snapshot.mouse_button_down[MouseButtons::Left] =
        ImGui::IsMouseDown(ImGuiMouseButton_Left);
    snapshot.mouse_button_down[MouseButtons::Middle] =
        ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    snapshot.mouse_button_down[MouseButtons::Right] =
        ImGui::IsMouseDown(ImGuiMouseButton_Right);
    snapshot.mouse_button_pressed[MouseButtons::Left] =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, false);
    snapshot.mouse_button_pressed[MouseButtons::Middle] =
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle, false);
    snapshot.mouse_button_pressed[MouseButtons::Right] =
        ImGui::IsMouseClicked(ImGuiMouseButton_Right, false);
    snapshot.mouse_button_released[MouseButtons::Left] =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    snapshot.mouse_button_released[MouseButtons::Middle] =
        ImGui::IsMouseReleased(ImGuiMouseButton_Middle);
    snapshot.mouse_button_released[MouseButtons::Right] =
        ImGui::IsMouseReleased(ImGuiMouseButton_Right);

    for (uint32_t i = 0; i < KeyButtons::KeyButtonCount; ++i) {
        const ImGuiKey imgui_key = ToImGuiKey(static_cast<KeyButtons>(i));
        if (imgui_key == ImGuiKey_None) {
            continue;
        }
        snapshot.key_down[i]     = ImGui::IsKeyDown(imgui_key);
        snapshot.key_pressed[i]  = ImGui::IsKeyPressed(imgui_key, false);
        snapshot.key_released[i] = ImGui::IsKeyReleased(imgui_key);
    }

    return snapshot;
}

} // namespace Moer::Render
