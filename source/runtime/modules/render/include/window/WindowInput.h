#ifndef MOERENGINE_WINDOW_INPUT_H
#define MOERENGINE_WINDOW_INPUT_H
#include "misc/STL.h"

namespace Moer {

    typedef enum {
        Left,
        Middle,
        Right,

        MouseButtonCount,
        MouseButtonFirst = Left,
    } MouseButtons;

    // restores the input information after processing (including camera control logic)
    struct WindowInput {

        // cursor
        float cursor_last_x  = 0.0f;
        float cursor_last_y  = 0.0f;
        float cursor_delta_x = 0.0f;
        float cursor_delta_y = 0.0f;

        bool is_cursor_dirty = true;// origin "firstMouse", presents if the cursor is needed to be reset (such as when you press F key 2 times)

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

        // camera speed(default)
        const float k_default_camera_speed    = 25.0f;
        const float k_max_camera_speed        = 100.0f;
        const float k_min_camera_speed        = 0.0f;
        const float k_camera_speed_up_delta   = 5.0f;
        const float k_camera_speed_down_delta = 2.5f;

        float camera_speed = k_default_camera_speed;//to be optimized
        bool  speed_up     = false;
        bool  speed_down   = false;
        bool  reset_speed  = false;

        // window size
        float width        = 1280.f;
        float height       = 720.f;
        float aspect_ratio = width / height;

        // fov
        float fov = 60.f;

        // mouse button state
        bool                                              is_cursor_hiding   = false;
        StaticArray<bool, MouseButtons::MouseButtonCount> mouse_button_state = {false};

        // singleton
        static WindowInput& GetInstance();
    };

    inline WindowInput& WindowInput::GetInstance() {
        static WindowInput wndInput;
        return wndInput;
    }
}// namespace Moer

#endif