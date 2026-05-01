#ifndef MOERENGINE_WINDOW_INPUT_H
#define MOERENGINE_WINDOW_INPUT_H

#include "RenderAPI.h"
#include "misc/STL.h"
#include "misc/Traits.h"

namespace Moer {

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

// restores the input information after processing (including camera control logic)

struct RENDER_API WindowInput {
    // inject
    bool  is_active = true; // camera input is captured by SceneColor or active through the F toggle
    uint2 m_scene_color_resolution;
    uint2 m_scene_color_pos;

    // cursor
    float cursor_last_x  = 0.0f;
    float cursor_last_y  = 0.0f;
    float cursor_delta_x = 0.0f;
    float cursor_delta_y = 0.0f;
    float scroll_offset  = 0.0f;

    bool is_cursor_dirty =
        true; // origin "firstMouse", presents if the cursor is needed to be reset (such as when you press F key 2 times)

    // timing
    float delta_time      = 0.0f;
    float last_frame_time = 0.0f;

    // camera movement
    bool camera_forward  = false;
    bool camera_backward = false;
    bool camera_left     = false;
    bool camera_right    = false;
    bool camera_up       = false;
    bool camera_down     = false;

    bool speed_up    = false;
    bool speed_down  = false;
    bool reset_speed = false;

    // window size
    float width        = 1280.f;
    float height       = 720.f;
    float aspect_ratio = width / height;

    // mouse button state
    bool is_cursor_hiding = false;

    StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_state      = {false};
    StaticArray<bool, KeyButtons::KeyButtonCount>     key_button_state        = {false}; // Press or Release
    StaticArray<bool, KeyButtons::KeyButtonCount>     key_button_switch_state = {
        false
    }; // Press once to switch state

    // singleton
    static WindowInput& Get();

    WindowInput(const WindowInput&)            = delete;
    WindowInput& operator=(const WindowInput&) = delete;

private:
    WindowInput() {}
    ~WindowInput() {}
};
} // namespace Moer

#endif