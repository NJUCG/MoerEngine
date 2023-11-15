#include "GLFWUIImpl.h"
#include "GLFW/glfw3.h"

namespace Moer {
    static ImGuiImplGlfwData* ImGuiImplGlfwGetBackendData() {
        return ImGui::GetCurrentContext() ? (ImGuiImplGlfwData*)ImGui::GetIO().BackendPlatformUserData : nullptr;
    }
    static bool ImGuiImplGlfwShouldChainCallback(GLFWwindow* window) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        return bd->callbacks_chain_for_all_windows ? true : (window == bd->window);
    }
    static int      ImGuiImplGlfwTranslateUntranslatedKey(int key, int scancode);
    static ImGuiKey ImGuiImplGlfwKeyToImGuiKey(int key);

    static void ImGuiImplGlfwUpdateKeyModifiers(GLFWwindow* window) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS));
        io.AddKeyEvent(ImGuiMod_Shift, (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS));
        io.AddKeyEvent(ImGuiMod_Alt, (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS));
        io.AddKeyEvent(ImGuiMod_Super, (glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS) || (glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS));
    }
    void ImGuiImplGlfwKeyCallback(GLFWwindow* window, int keycode, int scancode, int action, int mods) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_key != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_key(window, keycode, scancode, action, mods);

        if (action != GLFW_PRESS && action != GLFW_RELEASE)
            return;

        ImGuiImplGlfwUpdateKeyModifiers(window);

        if (keycode >= 0 && keycode < IM_ARRAYSIZE(bd->key_owner_windows))
            bd->key_owner_windows[keycode] = (action == GLFW_PRESS) ? window : nullptr;

        keycode = ImGuiImplGlfwTranslateUntranslatedKey(keycode, scancode);

        ImGuiIO& io        = ImGui::GetIO();
        ImGuiKey imgui_key = ImGuiImplGlfwKeyToImGuiKey(keycode);
        io.AddKeyEvent(imgui_key, (action == GLFW_PRESS));
        io.SetKeyEventNativeData(imgui_key, keycode, scancode);// To support legacy indexing (<1.87 user code)
    }

    void ImGuiImplGlfwWindowFocusCallback(GLFWwindow* window, int focused) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_window_focus != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_window_focus(window, focused);

        ImGuiIO& io = ImGui::GetIO();
        io.AddFocusEvent(focused != 0);
    }

    void ImGuiImplGlfwCursorPosCallback(GLFWwindow* window, double x, double y) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_cursor_pos != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_cursor_pos(window, x, y);
        if (glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            return;

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            int window_x, window_y;
            glfwGetWindowPos(window, &window_x, &window_y);
            x += window_x;
            y += window_y;
        }
        io.AddMousePosEvent((float)x, (float)y);
        bd->last_valid_mouse_pos = ImVec2((float)x, (float)y);
    }

    // Workaround: X11 seems to send spurious Leave/Enter events which would make us lose our position,
    // so we back it up and restore on Leave/Enter (see https://github.com/ocornut/imgui/issues/4984)
    void ImGuiImplGlfwCursorEnterCallback(GLFWwindow* window, int entered) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_cursor_enter != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_cursor_enter(window, entered);
        if (glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            return;

        ImGuiIO& io = ImGui::GetIO();
        if (entered) {
            bd->mouse_window = window;
            io.AddMousePosEvent(bd->last_valid_mouse_pos.x, bd->last_valid_mouse_pos.y);
        } else if (!entered && bd->mouse_window == window) {
            bd->last_valid_mouse_pos = io.MousePos;
            bd->mouse_window         = nullptr;
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
    }

    void ImGuiImplGlfwCharCallback(GLFWwindow* window, unsigned int c) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_char != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_char(window, c);

        ImGuiIO& io = ImGui::GetIO();
        io.AddInputCharacter(c);
    }

    void ImGuiImplGlfwMonitorCallback(GLFWmonitor*, int) {
        ImGuiImplGlfwData* bd    = ImGuiImplGlfwGetBackendData();
        bd->want_update_monitors = true;
    }
    void ImGuiImplGlfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_mousebutton != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_mousebutton(window, button, action, mods);

        ImGuiImplGlfwUpdateKeyModifiers(window);

        ImGuiIO& io = ImGui::GetIO();
        if (button >= 0 && button < ImGuiMouseButton_COUNT)
            io.AddMouseButtonEvent(button, action == GLFW_PRESS);
    }

    static const char* ImGuiImplGlfwGetClipboardText(void* user_data) {
        return glfwGetClipboardString((GLFWwindow*)user_data);
    }

    static void ImGuiImplGlfwSetClipboardText(void* user_data, const char* text) {
        glfwSetClipboardString((GLFWwindow*)user_data, text);
    }

    void ImGuiImplGlfwScrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (bd->prev_user_callback_scroll != nullptr && ImGuiImplGlfwShouldChainCallback(window))
            bd->prev_user_callback_scroll(window, xoffset, yoffset);

#ifdef __EMSCRIPTEN__
        // Ignore GLFW events: will be processed in ImGui_ImplEmscripten_WheelCallback().
        return;
#endif

        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseWheelEvent((float)xoffset, (float)yoffset);
    }

    void ImGuiImplGlfwInstallCallbacks(GLFWwindow* window) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        IM_ASSERT(bd->installed_callbacks == false && "Callbacks already installed!");
        IM_ASSERT(bd->window == window);

        bd->prev_user_callback_window_focus = glfwSetWindowFocusCallback(window, ImGuiImplGlfwWindowFocusCallback);
        bd->prev_user_callback_cursor_enter = glfwSetCursorEnterCallback(window, ImGuiImplGlfwCursorEnterCallback);
        bd->prev_user_callback_cursor_pos   = glfwSetCursorPosCallback(window, ImGuiImplGlfwCursorPosCallback);
        bd->prev_user_callback_mousebutton  = glfwSetMouseButtonCallback(window, ImGuiImplGlfwMouseButtonCallback);
        bd->prev_user_callback_scroll       = glfwSetScrollCallback(window, ImGuiImplGlfwScrollCallback);
        bd->prev_user_callback_key          = glfwSetKeyCallback(window, ImGuiImplGlfwKeyCallback);
        bd->prev_user_callback_char         = glfwSetCharCallback(window, ImGuiImplGlfwCharCallback);
        bd->prev_user_callback_monitor      = glfwSetMonitorCallback(ImGuiImplGlfwMonitorCallback);
        bd->installed_callbacks             = true;
    }
    static void ImGuiImplGlfwUpdateMonitors() {
        ImGuiImplGlfwData* bd             = ImGuiImplGlfwGetBackendData();
        ImGuiPlatformIO&   platform_io    = ImGui::GetPlatformIO();
        int                monitors_count = 0;
        GLFWmonitor**      glfw_monitors  = glfwGetMonitors(&monitors_count);
        platform_io.Monitors.resize(0);
        bd->want_update_monitors = false;
        for (int n = 0; n < monitors_count; n++) {
            ImGuiPlatformMonitor monitor;
            int                  x, y;
            glfwGetMonitorPos(glfw_monitors[n], &x, &y);
            const GLFWvidmode* vid_mode = glfwGetVideoMode(glfw_monitors[n]);
            if (vid_mode == nullptr)
                continue;// Failed to get Video mode (e.g. Emscripten does not support this function)
            monitor.MainPos = monitor.WorkPos = ImVec2((float)x, (float)y);
            monitor.MainSize = monitor.WorkSize = ImVec2((float)vid_mode->width, (float)vid_mode->height);
#if GLFW_HAS_MONITOR_WORK_AREA
            int w, h;
            glfwGetMonitorWorkarea(glfw_monitors[n], &x, &y, &w, &h);
            if (w > 0 && h > 0)// Workaround a small GLFW issue reporting zero on monitor changes: https://github.com/glfw/glfw/pull/1761
            {
                monitor.WorkPos  = ImVec2((float)x, (float)y);
                monitor.WorkSize = ImVec2((float)w, (float)h);
            }
#endif
#if GLFW_HAS_PER_MONITOR_DPI
            // Warning: the validity of monitor DPI information on Windows depends on the application DPI awareness settings, which generally needs to be set in the manifest or at runtime.
            float x_scale, y_scale;
            glfwGetMonitorContentScale(glfw_monitors[n], &x_scale, &y_scale);
            monitor.DpiScale = x_scale;
#endif
            monitor.PlatformHandle = (void*)glfw_monitors[n];// [...] GLFW doc states: "guaranteed to be valid only until the monitor configuration changes"
            platform_io.Monitors.push_back(monitor);
        }
    }
    static void ImGuiImplGlfwUpdateMouseData() {
        ImGuiImplGlfwData* bd          = ImGuiImplGlfwGetBackendData();
        ImGuiIO&           io          = ImGui::GetIO();
        ImGuiPlatformIO&   platform_io = ImGui::GetPlatformIO();

        if (glfwGetInputMode(bd->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            return;
        }

        ImGuiID      mouse_viewport_id = 0;
        const ImVec2 mouse_pos_prev    = io.MousePos;
        for (int n = 0; n < platform_io.Viewports.Size; n++) {
            ImGuiViewport* viewport = platform_io.Viewports[n];
            GLFWwindow*    window   = (GLFWwindow*)viewport->PlatformHandle;

#ifdef __EMSCRIPTEN__
            const bool is_window_focused = true;
#else
            const bool is_window_focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
#endif
            if (is_window_focused) {
                // (Optional) Set OS mouse position from Dear ImGui if requested (rarely used, only when ImGuiConfigFlags_NavEnableSetMousePos is enabled by user)
                // When multi-viewports are enabled, all Dear ImGui positions are same as OS positions.
                if (io.WantSetMousePos)
                    glfwSetCursorPos(window, (double)(mouse_pos_prev.x - viewport->Pos.x), (double)(mouse_pos_prev.y - viewport->Pos.y));

                // (Optional) Fallback to provide mouse position when focused (ImGui_ImplGlfw_CursorPosCallback already provides this when hovered or captured)
                if (bd->mouse_window == nullptr) {
                    double mouse_x, mouse_y;
                    glfwGetCursorPos(window, &mouse_x, &mouse_y);
                    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                        // Single viewport mode: mouse position in client window coordinates (io.MousePos is (0,0) when the mouse is on the upper-left corner of the app window)
                        // Multi-viewport mode: mouse position in OS absolute coordinates (io.MousePos is (0,0) when the mouse is on the upper-left of the primary monitor)
                        int window_x, window_y;
                        glfwGetWindowPos(window, &window_x, &window_y);
                        mouse_x += window_x;
                        mouse_y += window_y;
                    }
                    bd->last_valid_mouse_pos = ImVec2((float)mouse_x, (float)mouse_y);
                    io.AddMousePosEvent((float)mouse_x, (float)mouse_y);
                }
            }

            // (Optional) When using multiple viewports: call io.AddMouseViewportEvent() with the viewport the OS mouse cursor is hovering.
            // If ImGuiBackendFlags_HasMouseHoveredViewport is not set by the backend, Dear imGui will ignore this field and infer the information using its flawed heuristic.
            // - [X] GLFW >= 3.3 backend ON WINDOWS ONLY does correctly ignore viewports with the _NoInputs flag.
            // - [!] GLFW <= 3.2 backend CANNOT correctly ignore viewports with the _NoInputs flag, and CANNOT reported Hovered Viewport because of mouse capture.
            //       Some backend are not able to handle that correctly. If a backend report an hovered viewport that has the _NoInputs flag (e.g. when dragging a window
            //       for docking, the viewport has the _NoInputs flag in order to allow us to find the viewport under), then Dear ImGui is forced to ignore the value reported
            //       by the backend, and use its flawed heuristic to guess the viewport behind.
            // - [X] GLFW backend correctly reports this regardless of another viewport behind focused and dragged from (we need this to find a useful drag and drop target).
            // FIXME: This is currently only correct on Win32. See what we do below with the WM_NCHITTEST, missing an equivalent for other systems.
            // See https://github.com/glfw/glfw/issues/1236 if you want to help in making this a GLFW feature.
#if GLFW_HAS_MOUSE_PASSTHROUGH || (GLFW_HAS_WINDOW_HOVERED && defined(_WIN32))
            const bool window_no_input = (viewport->Flags & ImGuiViewportFlags_NoInputs) != 0;
#if GLFW_HAS_MOUSE_PASSTHROUGH
            glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH, window_no_input);
#endif
            if (glfwGetWindowAttrib(window, GLFW_HOVERED) && !window_no_input)
                mouse_viewport_id = viewport->ID;
#else
            // We cannot use bd->MouseWindow maintained from CursorEnter/Leave callbacks, because it is locked to the window capturing mouse.
#endif
        }

        if (io.BackendFlags & ImGuiBackendFlags_HasMouseHoveredViewport)
            io.AddMouseViewportEvent(mouse_viewport_id);
    }
    static void ImGuiImplGlfwUpdateMouseCursor() {
        ImGuiIO&           io = ImGui::GetIO();
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) || glfwGetInputMode(bd->window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED)
            return;

        ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
        ImGuiPlatformIO& platform_io  = ImGui::GetPlatformIO();
        for (int n = 0; n < platform_io.Viewports.Size; n++) {
            GLFWwindow* window = (GLFWwindow*)platform_io.Viewports[n]->PlatformHandle;
            if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
                // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
            } else {
                // Show OS mouse cursor
                // FIXME-PLATFORM: Unfocused windows seems to fail changing the mouse cursor with GLFW 3.2, but 3.3 works here.
                glfwSetCursor(window, bd->mouse_cursors[imgui_cursor] ? bd->mouse_cursors[imgui_cursor] : bd->mouse_cursors[ImGuiMouseCursor_Arrow]);
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }
        }
    }

    // Update gamepad inputs
    static inline float Saturate(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f :
                                                                                v; }
    static void         ImGuiImplGlfwUpdateGamepads() {
        ImGuiIO& io = ImGui::GetIO();
        if ((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) == 0)// FIXME: Technically feeding gamepad shouldn't depend on this now that they are regular inputs.
            return;

        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
#if GLFW_HAS_GAMEPAD_API && !defined(__EMSCRIPTEN__)
        GLFWgamepadstate gamepad;
        if (!glfwGetGamepadState(GLFW_JOYSTICK_1, &gamepad))
            return;
#define MAP_BUTTON(KEY_NO, BUTTON_NO, _UNUSED) \
    do { io.AddKeyEvent(KEY_NO, gamepad.buttons[BUTTON_NO] != 0); } while (0)
#define MAP_ANALOG(KEY_NO, AXIS_NO, _UNUSED, V0, V1)          \
    do {                                                      \
        float v = gamepad.axes[AXIS_NO];                      \
        v       = (v - V0) / (V1 - V0);                       \
        io.AddKeyAnalogEvent(KEY_NO, v > 0.10f, Saturate(v)); \
    } while (0)
#else
        int                  axes_count = 0, buttons_count = 0;
        const float*         axes    = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axes_count);
        const unsigned char* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &buttons_count);
        if (axes_count == 0 || buttons_count == 0)
            return;
#define MAP_BUTTON(KEY_NO, _UNUSED, BUTTON_NO) \
    do { io.AddKeyEvent(KEY_NO, (buttons_count > BUTTON_NO && buttons[BUTTON_NO] == GLFW_PRESS)); } while (0)
#define MAP_ANALOG(KEY_NO, _UNUSED, AXIS_NO, V0, V1)           \
    do {                                                       \
        float v = (axes_count > AXIS_NO) ? axes[AXIS_NO] : V0; \
        v       = (v - V0) / (V1 - V0);                        \
        io.AddKeyAnalogEvent(KEY_NO, v > 0.10f, Saturate(v));  \
    } while (0)
#endif
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        MAP_BUTTON(ImGuiKey_GamepadStart, GLFW_GAMEPAD_BUTTON_START, 7);
        MAP_BUTTON(ImGuiKey_GamepadBack, GLFW_GAMEPAD_BUTTON_BACK, 6);
        MAP_BUTTON(ImGuiKey_GamepadFaceLeft, GLFW_GAMEPAD_BUTTON_X, 2); // Xbox X, PS Square
        MAP_BUTTON(ImGuiKey_GamepadFaceRight, GLFW_GAMEPAD_BUTTON_B, 1);// Xbox B, PS Circle
        MAP_BUTTON(ImGuiKey_GamepadFaceUp, GLFW_GAMEPAD_BUTTON_Y, 3);   // Xbox Y, PS Triangle
        MAP_BUTTON(ImGuiKey_GamepadFaceDown, GLFW_GAMEPAD_BUTTON_A, 0); // Xbox A, PS Cross
        MAP_BUTTON(ImGuiKey_GamepadDpadLeft, GLFW_GAMEPAD_BUTTON_DPAD_LEFT, 13);
        MAP_BUTTON(ImGuiKey_GamepadDpadRight, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, 11);
        MAP_BUTTON(ImGuiKey_GamepadDpadUp, GLFW_GAMEPAD_BUTTON_DPAD_UP, 10);
        MAP_BUTTON(ImGuiKey_GamepadDpadDown, GLFW_GAMEPAD_BUTTON_DPAD_DOWN, 12);
        MAP_BUTTON(ImGuiKey_GamepadL1, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, 4);
        MAP_BUTTON(ImGuiKey_GamepadR1, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, 5);
        MAP_ANALOG(ImGuiKey_GamepadL2, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, 4, -0.75f, +1.0f);
        MAP_ANALOG(ImGuiKey_GamepadR2, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, 5, -0.75f, +1.0f);
        MAP_BUTTON(ImGuiKey_GamepadL3, GLFW_GAMEPAD_BUTTON_LEFT_THUMB, 8);
        MAP_BUTTON(ImGuiKey_GamepadR3, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, 9);
        MAP_ANALOG(ImGuiKey_GamepadLStickLeft, GLFW_GAMEPAD_AXIS_LEFT_X, 0, -0.25f, -1.0f);
        MAP_ANALOG(ImGuiKey_GamepadLStickRight, GLFW_GAMEPAD_AXIS_LEFT_X, 0, +0.25f, +1.0f);
        MAP_ANALOG(ImGuiKey_GamepadLStickUp, GLFW_GAMEPAD_AXIS_LEFT_Y, 1, -0.25f, -1.0f);
        MAP_ANALOG(ImGuiKey_GamepadLStickDown, GLFW_GAMEPAD_AXIS_LEFT_Y, 1, +0.25f, +1.0f);
        MAP_ANALOG(ImGuiKey_GamepadRStickLeft, GLFW_GAMEPAD_AXIS_RIGHT_X, 2, -0.25f, -1.0f);
        MAP_ANALOG(ImGuiKey_GamepadRStickRight, GLFW_GAMEPAD_AXIS_RIGHT_X, 2, +0.25f, +1.0f);
        MAP_ANALOG(ImGuiKey_GamepadRStickUp, GLFW_GAMEPAD_AXIS_RIGHT_Y, 3, -0.25f, -1.0f);
        MAP_ANALOG(ImGuiKey_GamepadRStickDown, GLFW_GAMEPAD_AXIS_RIGHT_Y, 3, +0.25f, +1.0f);
#undef MAP_BUTTON
#undef MAP_ANALOG
    }
#ifdef _WIN32
    // GLFW doesn't allow to distinguish Mouse vs TouchScreen vs Pen.
    // Add support for Win32 (based on imgui_impl_win32), because we rely on _TouchScreen info to trickle inputs differently.
    static ImGuiMouseSource GetMouseSourceFromMessageExtraInfo() {
        LPARAM extra_info = ::GetMessageExtraInfo();
        if ((extra_info & 0xFFFFFF80) == 0xFF515700)
            return ImGuiMouseSource_Pen;
        if ((extra_info & 0xFFFFFF80) == 0xFF515780)
            return ImGuiMouseSource_TouchScreen;
        return ImGuiMouseSource_Mouse;
    }
    static LRESULT CALLBACK ImGuiImplGlfwWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        switch (msg) {
            case WM_MOUSEMOVE:
            case WM_NCMOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONDBLCLK:
            case WM_XBUTTONUP:
                ImGui::GetIO().AddMouseSourceEvent(GetMouseSourceFromMessageExtraInfo());
                break;

                // We have submitted https://github.com/glfw/glfw/pull/1568 to allow GLFW to support "transparent inputs".
                // In the meanwhile we implement custom per-platform workarounds here (FIXME-VIEWPORT: Implement same work-around for Linux/OSX!)
#if !GLFW_HAS_MOUSE_PASSTHROUGH && GLFW_HAS_WINDOW_HOVERED
            case WM_NCHITTEST: {
                // Let mouse pass-through the window. This will allow the backend to call io.AddMouseViewportEvent() properly (which is OPTIONAL).
                // The ImGuiViewportFlags_NoInputs flag is set while dragging a viewport, as want to detect the window behind the one we are dragging.
                // If you cannot easily access those viewport flags from your windowing/event code: you may manually synchronize its state e.g. in
                // your main loop after calling UpdatePlatformWindows(). Iterate all viewports/platform windows and pass the flag to your windowing system.
                ImGuiViewport* viewport = (ImGuiViewport*)::GetPropA(hWnd, "IMGUI_VIEWPORT");
                if (viewport && (viewport->Flags & ImGuiViewportFlags_NoInputs))
                    return HTTRANSPARENT;
                break;
            }
#endif
        }
        return ::CallWindowProc(bd->glfw_wnd_proc, hWnd, msg, wParam, lParam);
    }
#endif
    static void   ImGuiImplGlfwCreateWindow(ImGuiViewport* viewport);
    static void   ImGuiImplGlfwDestroyWindow(ImGuiViewport* viewport);
    static void   ImGuiImplGlfwShowWindow(ImGuiViewport* viewport);
    static ImVec2 ImGuiImplGlfwGetWindowPos(ImGuiViewport* viewport);
    static void   ImGuiImplGlfwSetWindowPos(ImGuiViewport* viewport, ImVec2 pos);
    static ImVec2 ImGuiImplGlfwGetWindowSize(ImGuiViewport* viewport);
    static void   ImGuiImplGlfwSetWindowSize(ImGuiViewport* viewport, ImVec2 size);
    static void   ImGuiImplGlfwSetWindowTitle(ImGuiViewport* viewport, const char* title);
    static void   ImGuiImplGlfwSetWindowFocus(ImGuiViewport* viewport);
    static bool   ImGuiImplGlfwGetWindowFocus(ImGuiViewport* viewport);
    static bool   ImGuiImplGlfwGetWindowMinimized(ImGuiViewport* viewport);
    static void   ImGuiImplGlfwRenderWindow(ImGuiViewport* viewport, void*);
    static void   ImGuiImplGlfwSwapBuffers(ImGuiViewport* viewport, void*);
#if GLFW_HAS_WINDOW_ALPHA
    static void ImGuiImplGlfwSetWindowAlpha(ImGuiViewport* viewport, float alpha) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        glfwSetWindowOpacity(vd->window, alpha);
    }
#endif
    static void ImGuiImplGlfwInitPlatformInterface() {
        // Register platform interface (will be coupled with a renderer interface)
        ImGuiImplGlfwData* bd                   = ImGuiImplGlfwGetBackendData();
        ImGuiPlatformIO&   platform_io          = ImGui::GetPlatformIO();
        platform_io.Platform_CreateWindow       = ImGuiImplGlfwCreateWindow;
        platform_io.Platform_DestroyWindow      = ImGuiImplGlfwDestroyWindow;
        platform_io.Platform_ShowWindow         = ImGuiImplGlfwShowWindow;
        platform_io.Platform_SetWindowPos       = ImGuiImplGlfwSetWindowPos;
        platform_io.Platform_GetWindowPos       = ImGuiImplGlfwGetWindowPos;
        platform_io.Platform_SetWindowSize      = ImGuiImplGlfwSetWindowSize;
        platform_io.Platform_GetWindowSize      = ImGuiImplGlfwGetWindowSize;
        platform_io.Platform_SetWindowFocus     = ImGuiImplGlfwSetWindowFocus;
        platform_io.Platform_GetWindowFocus     = ImGuiImplGlfwGetWindowFocus;
        platform_io.Platform_GetWindowMinimized = ImGuiImplGlfwGetWindowMinimized;
        platform_io.Platform_SetWindowTitle     = ImGuiImplGlfwSetWindowTitle;
        platform_io.Platform_RenderWindow       = ImGuiImplGlfwRenderWindow;
        platform_io.Platform_SwapBuffers        = ImGuiImplGlfwSwapBuffers;
#if GLFW_HAS_WINDOW_ALPHA
        platform_io.Platform_SetWindowAlpha = ImGuiImplGlfwSetWindowAlpha;
#endif

        // Register main window handle (which is owned by the main application, not by us)
        // This is mostly for simplicity and consistency, so that our code (e.g. mouse handling etc.) can use same logic for main and secondary viewports.
        ImGuiViewport*             main_viewport = ImGui::GetMainViewport();
        ImGuiImplGlfwViewportData* vd            = IM_NEW(ImGuiImplGlfwViewportData)();
        vd->window                               = bd->window;
        vd->window_owned                         = false;
        main_viewport->PlatformUserData          = vd;
        main_viewport->PlatformHandle            = (void*)bd->window;
    }

    void GuiWindowInit(const GuiWindowInitInfo& _info) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // io.ConfigDockingAlwaysTabBar         = true;
        // io.ConfigWindowsMoveFromTitleBarOnly = true;

        IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");
        //printf("GLFW_VERSION: %d.%d.%d (%d)", GLFW_VERSION_MAJOR, GLFW_VERSION_MINOR, GLFW_VERSION_REVISION, GLFW_VERSION_COMBINED);

        // Setup backend capabilities flags
        ImGuiImplGlfwData* bd      = IM_NEW(ImGuiImplGlfwData)();
        io.BackendPlatformUserData = (void*)bd;
        io.BackendPlatformName     = "imgui_impl_glfw";
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;// We can honor GetMouseCursor() values (optional)
        io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos; // We can honor io.WantSetMousePos requests (optional, rarely used)
#ifndef __EMSCRIPTEN__
        io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;// We can create multi-viewports on the Platform side (optional)
#endif
#if GLFW_HAS_MOUSE_PASSTHROUGH || (GLFW_HAS_WINDOW_HOVERED && defined(_WIN32))
        io.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;// We can call io.AddMouseViewportEvent() with correct data (optional)
#endif

        bd->window               = (GLFWwindow*)_info.window;
        bd->time                 = 0.0;
        bd->want_update_monitors = true;

        io.SetClipboardTextFn = ImGuiImplGlfwSetClipboardText;
        io.GetClipboardTextFn = ImGuiImplGlfwGetClipboardText;
        io.ClipboardUserData  = bd->window;

        // Create mouse cursors
        // (By design, on X11 cursors are user configurable and some cursors may be missing. When a cursor doesn't exist,
        // GLFW will emit an error which will often be printed by the app, so we temporarily disable error reporting.
        // Missing cursors will return nullptr and our _UpdateMouseCursor() function will use the Arrow cursor instead.)
        GLFWerrorfun prev_error_callback              = glfwSetErrorCallback(nullptr);
        bd->mouse_cursors[ImGuiMouseCursor_Arrow]     = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_TextInput] = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_ResizeNS]  = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_ResizeEW]  = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_Hand]      = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
#if GLFW_HAS_NEW_CURSORS
        bd->MouseCursors[ImGuiMouseCursor_ResizeAll]  = glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR);
        bd->MouseCursors[ImGuiMouseCursor_ResizeNESW] = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);
        bd->MouseCursors[ImGuiMouseCursor_ResizeNWSE] = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
        bd->MouseCursors[ImGuiMouseCursor_NotAllowed] = glfwCreateStandardCursor(GLFW_NOT_ALLOWED_CURSOR);
#else
        bd->mouse_cursors[ImGuiMouseCursor_ResizeAll]  = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_ResizeNESW] = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_ResizeNWSE] = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
        bd->mouse_cursors[ImGuiMouseCursor_NotAllowed] = glfwCreateStandardCursor(GLFW_ARROW_CURSOR);
#endif
        glfwSetErrorCallback(prev_error_callback);
#if GLFW_HAS_GETERROR && !defined(__EMSCRIPTEN__)// Eat errors (see #5908)
        (void)glfwGetError(nullptr);
#endif

        // Chain GLFW callbacks: our callbacks will call the user's previously installed callbacks, if any.

        ImGuiImplGlfwInstallCallbacks((GLFWwindow*)_info.window);
        // Register Emscripten Wheel callback to workaround issue in Emscripten GLFW Emulation (#6096)
        // We intentionally do not check 'if (install_callbacks)' here, as some users may set it to false and call GLFW callback themselves.
        // FIXME: May break chaining in case user registered their own Emscripten callback?
#ifdef __EMSCRIPTEN__
        emscripten_set_wheel_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, false, ImGui_ImplEmscripten_WheelCallback);
#endif

        // Update monitors the first time (note: monitor callback are broken in GLFW 3.2 and earlier, see github.com/glfw/glfw/issues/784)
        ImGuiImplGlfwUpdateMonitors();
        glfwSetMonitorCallback(ImGuiImplGlfwMonitorCallback);

        // Set platform dependent data in viewport
        ImGuiViewport* main_viewport  = ImGui::GetMainViewport();
        main_viewport->PlatformHandle = (void*)bd->window;
#ifdef _WIN32
        main_viewport->PlatformHandleRaw = glfwGetWin32Window(bd->window);
#elif defined(__APPLE__)
        main_viewport->PlatformHandleRaw               = (void*)glfwGetCocoaWindow(bd->Window);
#else
        IM_UNUSED(main_viewport);
#endif
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            ImGuiImplGlfwInitPlatformInterface();

            // Windows: register a WndProc hook so we can intercept some messages.
#ifdef _WIN32
        bd->glfw_wnd_proc = (WNDPROC)::GetWindowLongPtr((HWND)main_viewport->PlatformHandleRaw, GWLP_WNDPROC);
        IM_ASSERT(bd->glfw_wnd_proc != nullptr);
        ::SetWindowLongPtr((HWND)main_viewport->PlatformHandleRaw, GWLP_WNDPROC, (LONG_PTR)ImGuiImplGlfwWndProc);
#endif

        bd->client_api = _info.rhi_type;
    }
    void ImGuiImplGlfwRestoreCallbacks(GLFWwindow* window);

    void GuiWindowShutDown() {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        IM_ASSERT(bd != nullptr && "No platform backend to shutdown, or already shutdown?");
        ImGuiIO& io = ImGui::GetIO();

        ImGui::DestroyPlatformWindows();

        if (bd->installed_callbacks)
            ImGuiImplGlfwRestoreCallbacks(bd->window);

        for (ImGuiMouseCursor cursor_n = 0; cursor_n < ImGuiMouseCursor_COUNT; cursor_n++)
            glfwDestroyCursor(bd->mouse_cursors[cursor_n]);

            // Windows: register a WndProc hook so we can intercept some messages.
#ifdef _WIN32
        ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        ::SetWindowLongPtr((HWND)main_viewport->PlatformHandleRaw, GWLP_WNDPROC, (LONG_PTR)bd->glfw_wnd_proc);
        bd->glfw_wnd_proc = nullptr;
#endif

        io.BackendPlatformName     = nullptr;
        io.BackendPlatformUserData = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos | ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport);
        IM_DELETE(bd);
    }

    void GuiWindowNewFrame() {
        ImGuiIO&           io = ImGui::GetIO();
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplGlfw_InitForXXX()?");

        // Setup display size (every frame to accommodate for window resizing)
        int w, h;
        int display_w, display_h;
        glfwGetWindowSize(bd->window, &w, &h);
        glfwGetFramebufferSize(bd->window, &display_w, &display_h);
        io.DisplaySize = ImVec2((float)w, (float)h);
        if (w > 0 && h > 0)
            io.DisplayFramebufferScale = ImVec2((float)display_w / (float)w, (float)display_h / (float)h);
        if (bd->want_update_monitors)
            ImGuiImplGlfwUpdateMonitors();

        // Setup time step
        double current_time = glfwGetTime();
        io.DeltaTime        = bd->time > 0.0 ? (float)(current_time - bd->time) : (float)(1.0f / 60.0f);
        bd->time            = current_time;

        ImGuiImplGlfwUpdateMouseData();
        ImGuiImplGlfwUpdateMouseCursor();

        // Update game controllers (if enabled and available)
        ImGuiImplGlfwUpdateGamepads();
    }
#pragma region utils

    int ImGuiImplGlfwTranslateUntranslatedKey(int key, int scancode) {
#if GLFW_HAS_GETKEYNAME && !defined(__EMSCRIPTEN__)
        // GLFW 3.1+ attempts to "untranslate" keys, which goes the opposite of what every other framework does, making using lettered shortcuts difficult.
        // (It had reasons to do so: namely GLFW is/was more likely to be used for WASD-type game controls rather than lettered shortcuts, but IHMO the 3.1 change could have been done differently)
        // See https://github.com/glfw/glfw/issues/1502 for details.
        // Adding a workaround to undo this (so our keys are translated->untranslated->translated, likely a lossy process).
        // This won't cover edge cases but this is at least going to cover common cases.
        if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_EQUAL)
            return key;
        GLFWerrorfun prev_error_callback = glfwSetErrorCallback(nullptr);
        const char*  key_name            = glfwGetKeyName(key, scancode);
        glfwSetErrorCallback(prev_error_callback);
#if GLFW_HAS_GETERROR && !defined(__EMSCRIPTEN__)// Eat errors (see #5908)
        (void)glfwGetError(nullptr);
#endif
        if (key_name && key_name[0] != 0 && key_name[1] == 0) {
            const char char_names[] = "`-=[]\\,;\'./";
            const int  char_keys[]  = {GLFW_KEY_GRAVE_ACCENT, GLFW_KEY_MINUS, GLFW_KEY_EQUAL, GLFW_KEY_LEFT_BRACKET, GLFW_KEY_RIGHT_BRACKET, GLFW_KEY_BACKSLASH, GLFW_KEY_COMMA, GLFW_KEY_SEMICOLON, GLFW_KEY_APOSTROPHE, GLFW_KEY_PERIOD, GLFW_KEY_SLASH, 0};
            IM_ASSERT(IM_ARRAYSIZE(char_names) == IM_ARRAYSIZE(char_keys));
            if (key_name[0] >= '0' && key_name[0] <= '9') {
                key = GLFW_KEY_0 + (key_name[0] - '0');
            } else if (key_name[0] >= 'A' && key_name[0] <= 'Z') {
                key = GLFW_KEY_A + (key_name[0] - 'A');
            } else if (key_name[0] >= 'a' && key_name[0] <= 'z') {
                key = GLFW_KEY_A + (key_name[0] - 'a');
            } else if (const char* p = strchr(char_names, key_name[0])) {
                key = char_keys[p - char_names];
            }
        }
        // if (action == GLFW_PRESS) printf("key %d scancode %d name '%s'\n", key, scancode, key_name);
#else
        IM_UNUSED(scancode);
#endif
        return key;
    }

    ImGuiKey ImGuiImplGlfwKeyToImGuiKey(int key) {
        switch (key) {
            case GLFW_KEY_TAB: return ImGuiKey_Tab;
            case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
            case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
            case GLFW_KEY_UP: return ImGuiKey_UpArrow;
            case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
            case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
            case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
            case GLFW_KEY_HOME: return ImGuiKey_Home;
            case GLFW_KEY_END: return ImGuiKey_End;
            case GLFW_KEY_INSERT: return ImGuiKey_Insert;
            case GLFW_KEY_DELETE: return ImGuiKey_Delete;
            case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
            case GLFW_KEY_SPACE: return ImGuiKey_Space;
            case GLFW_KEY_ENTER: return ImGuiKey_Enter;
            case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
            case GLFW_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
            case GLFW_KEY_COMMA: return ImGuiKey_Comma;
            case GLFW_KEY_MINUS: return ImGuiKey_Minus;
            case GLFW_KEY_PERIOD: return ImGuiKey_Period;
            case GLFW_KEY_SLASH: return ImGuiKey_Slash;
            case GLFW_KEY_SEMICOLON: return ImGuiKey_Semicolon;
            case GLFW_KEY_EQUAL: return ImGuiKey_Equal;
            case GLFW_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
            case GLFW_KEY_BACKSLASH: return ImGuiKey_Backslash;
            case GLFW_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
            case GLFW_KEY_GRAVE_ACCENT: return ImGuiKey_GraveAccent;
            case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
            case GLFW_KEY_SCROLL_LOCK: return ImGuiKey_ScrollLock;
            case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
            case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
            case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
            case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
            case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
            case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
            case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
            case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
            case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
            case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
            case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
            case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
            case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
            case GLFW_KEY_KP_DECIMAL: return ImGuiKey_KeypadDecimal;
            case GLFW_KEY_KP_DIVIDE: return ImGuiKey_KeypadDivide;
            case GLFW_KEY_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
            case GLFW_KEY_KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
            case GLFW_KEY_KP_ADD: return ImGuiKey_KeypadAdd;
            case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
            case GLFW_KEY_KP_EQUAL: return ImGuiKey_KeypadEqual;
            case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
            case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
            case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
            case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
            case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
            case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
            case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
            case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
            case GLFW_KEY_MENU: return ImGuiKey_Menu;
            case GLFW_KEY_0: return ImGuiKey_0;
            case GLFW_KEY_1: return ImGuiKey_1;
            case GLFW_KEY_2: return ImGuiKey_2;
            case GLFW_KEY_3: return ImGuiKey_3;
            case GLFW_KEY_4: return ImGuiKey_4;
            case GLFW_KEY_5: return ImGuiKey_5;
            case GLFW_KEY_6: return ImGuiKey_6;
            case GLFW_KEY_7: return ImGuiKey_7;
            case GLFW_KEY_8: return ImGuiKey_8;
            case GLFW_KEY_9: return ImGuiKey_9;
            case GLFW_KEY_A: return ImGuiKey_A;
            case GLFW_KEY_B: return ImGuiKey_B;
            case GLFW_KEY_C: return ImGuiKey_C;
            case GLFW_KEY_D: return ImGuiKey_D;
            case GLFW_KEY_E: return ImGuiKey_E;
            case GLFW_KEY_F: return ImGuiKey_F;
            case GLFW_KEY_G: return ImGuiKey_G;
            case GLFW_KEY_H: return ImGuiKey_H;
            case GLFW_KEY_I: return ImGuiKey_I;
            case GLFW_KEY_J: return ImGuiKey_J;
            case GLFW_KEY_K: return ImGuiKey_K;
            case GLFW_KEY_L: return ImGuiKey_L;
            case GLFW_KEY_M: return ImGuiKey_M;
            case GLFW_KEY_N: return ImGuiKey_N;
            case GLFW_KEY_O: return ImGuiKey_O;
            case GLFW_KEY_P: return ImGuiKey_P;
            case GLFW_KEY_Q: return ImGuiKey_Q;
            case GLFW_KEY_R: return ImGuiKey_R;
            case GLFW_KEY_S: return ImGuiKey_S;
            case GLFW_KEY_T: return ImGuiKey_T;
            case GLFW_KEY_U: return ImGuiKey_U;
            case GLFW_KEY_V: return ImGuiKey_V;
            case GLFW_KEY_W: return ImGuiKey_W;
            case GLFW_KEY_X: return ImGuiKey_X;
            case GLFW_KEY_Y: return ImGuiKey_Y;
            case GLFW_KEY_Z: return ImGuiKey_Z;
            case GLFW_KEY_F1: return ImGuiKey_F1;
            case GLFW_KEY_F2: return ImGuiKey_F2;
            case GLFW_KEY_F3: return ImGuiKey_F3;
            case GLFW_KEY_F4: return ImGuiKey_F4;
            case GLFW_KEY_F5: return ImGuiKey_F5;
            case GLFW_KEY_F6: return ImGuiKey_F6;
            case GLFW_KEY_F7: return ImGuiKey_F7;
            case GLFW_KEY_F8: return ImGuiKey_F8;
            case GLFW_KEY_F9: return ImGuiKey_F9;
            case GLFW_KEY_F10: return ImGuiKey_F10;
            case GLFW_KEY_F11: return ImGuiKey_F11;
            case GLFW_KEY_F12: return ImGuiKey_F12;
            default: return ImGuiKey_None;
        }
    }
    static void ImGuiImplGlfwWindowCloseCallback(GLFWwindow* window) {
        if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window))
            viewport->PlatformRequestClose = true;
    }

    // GLFW may dispatch window pos/size events after calling glfwSetWindowPos()/glfwSetWindowSize().
    // However: depending on the platform the callback may be invoked at different time:
    // - on Windows it appears to be called within the glfwSetWindowPos()/glfwSetWindowSize() call
    // - on Linux it is queued and invoked during glfwPollEvents()
    // Because the event doesn't always fire on glfwSetWindowXXX() we use a frame counter tag to only
    // ignore recent glfwSetWindowXXX() calls.
    static void ImGuiImplGlfwWindowPosCallback(GLFWwindow* window, int, int) {
        if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window)) {
            if (ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData) {
                bool ignore_event = (ImGui::GetFrameCount() <= vd->ignore_window_pos_event_frame + 1);
                //data->IgnoreWindowPosEventFrame = -1;
                if (ignore_event)
                    return;
            }
            viewport->PlatformRequestMove = true;
        }
    }

    static void ImGuiImplGlfwWindowSizeCallback(GLFWwindow* window, int, int) {
        if (ImGuiViewport* viewport = ImGui::FindViewportByPlatformHandle(window)) {
            if (ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData) {
                bool ignore_event = (ImGui::GetFrameCount() <= vd->ignore_window_size_event_frame + 1);
                //data->IgnoreWindowSizeEventFrame = -1;
                if (ignore_event)
                    return;
            }
            viewport->PlatformRequestResize = true;
        }
    }
    void ImGuiImplGlfwCreateWindow(ImGuiViewport* viewport) {
        ImGuiImplGlfwData*         bd = ImGuiImplGlfwGetBackendData();
        ImGuiImplGlfwViewportData* vd = IM_NEW(ImGuiImplGlfwViewportData)();
        viewport->PlatformUserData    = vd;

        // GLFW 3.2 unfortunately always set focus on glfwCreateWindow() if GLFW_VISIBLE is set, regardless of GLFW_FOCUSED
        // With GLFW 3.3, the hint GLFW_FOCUS_ON_SHOW fixes this problem
        glfwWindowHint(GLFW_VISIBLE, false);
        glfwWindowHint(GLFW_FOCUSED, false);
#if GLFW_HAS_FOCUS_ON_SHOW
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, false);
#endif
        glfwWindowHint(GLFW_DECORATED, (viewport->Flags & ImGuiViewportFlags_NoDecoration) ? false : true);
#if GLFW_HAS_WINDOW_TOPMOST
        glfwWindowHint(GLFW_FLOATING, (viewport->Flags & ImGuiViewportFlags_TopMost) ? true : false);
#endif
        GLFWwindow* share_window = nullptr;
        vd->window               = glfwCreateWindow((int)viewport->Size.x, (int)viewport->Size.y, "No Title Yet", nullptr, share_window);
        vd->window_owned         = true;
        viewport->PlatformHandle = (void*)vd->window;
#ifdef _WIN32
        viewport->PlatformHandleRaw = glfwGetWin32Window(vd->window);
#elif defined(__APPLE__)
        viewport->PlatformHandleRaw = (void*)glfwGetCocoaWindow(vd->Window);
#endif
        glfwSetWindowPos(vd->window, (int)viewport->Pos.x, (int)viewport->Pos.y);

        // Install GLFW callbacks for secondary viewports
        glfwSetWindowFocusCallback(vd->window, ImGuiImplGlfwWindowFocusCallback);
        glfwSetCursorEnterCallback(vd->window, ImGuiImplGlfwCursorEnterCallback);
        glfwSetCursorPosCallback(vd->window, ImGuiImplGlfwCursorPosCallback);
        glfwSetMouseButtonCallback(vd->window, ImGuiImplGlfwMouseButtonCallback);
        glfwSetScrollCallback(vd->window, ImGuiImplGlfwScrollCallback);
        glfwSetKeyCallback(vd->window, ImGuiImplGlfwKeyCallback);
        glfwSetCharCallback(vd->window, ImGuiImplGlfwCharCallback);
        glfwSetWindowCloseCallback(vd->window, ImGuiImplGlfwWindowCloseCallback);
        glfwSetWindowPosCallback(vd->window, ImGuiImplGlfwWindowPosCallback);
        glfwSetWindowSizeCallback(vd->window, ImGuiImplGlfwWindowSizeCallback);
    }

    void ImGuiImplGlfwDestroyWindow(ImGuiViewport* viewport) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        if (ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData) {
            if (vd->window_owned) {
#if !GLFW_HAS_MOUSE_PASSTHROUGH && GLFW_HAS_WINDOW_HOVERED && defined(_WIN32)
                HWND hwnd = (HWND)viewport->PlatformHandleRaw;
                ::RemovePropA(hwnd, "IMGUI_VIEWPORT");
#endif

                // Release any keys that were pressed in the window being destroyed and are still held down,
                // because we will not receive any release events after window is destroyed.
                for (int i = 0; i < IM_ARRAYSIZE(bd->key_owner_windows); i++)
                    if (bd->key_owner_windows[i] == vd->window)
                        ImGuiImplGlfwKeyCallback(vd->window, i, 0, GLFW_RELEASE, 0);// Later params are only used for main viewport, on which this function is never called.

                glfwDestroyWindow(vd->window);
            }
            vd->window = nullptr;
            IM_DELETE(vd);
        }
        viewport->PlatformUserData = viewport->PlatformHandle = nullptr;
    }
    void ImGuiImplGlfwShowWindow(ImGuiViewport* viewport) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;

#if defined(_WIN32)
        // GLFW hack: Hide icon from task bar
        HWND hwnd = (HWND)viewport->PlatformHandleRaw;
        if (viewport->Flags & ImGuiViewportFlags_NoTaskBarIcon) {
            LONG ex_style = ::GetWindowLong(hwnd, GWL_EXSTYLE);
            ex_style &= ~WS_EX_APPWINDOW;
            ex_style |= WS_EX_TOOLWINDOW;
            ::SetWindowLong(hwnd, GWL_EXSTYLE, ex_style);
        }

        // GLFW hack: install hook for WM_NCHITTEST message handler
#if !GLFW_HAS_MOUSE_PASSTHROUGH && GLFW_HAS_WINDOW_HOVERED && defined(_WIN32)
        ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
        ::SetPropA(hwnd, "IMGUI_VIEWPORT", viewport);
        IM_ASSERT(bd->GlfwWndProc == (WNDPROC)::GetWindowLongPtr(hwnd, GWLP_WNDPROC));
        ::SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)ImGui_ImplGlfw_WndProc);
#endif

#if !GLFW_HAS_FOCUS_ON_SHOW
        // GLFW hack: GLFW 3.2 has a bug where glfwShowWindow() also activates/focus the window.
        // The fix was pushed to GLFW repository on 2018/01/09 and should be included in GLFW 3.3 via a GLFW_FOCUS_ON_SHOW window attribute.
        // See https://github.com/glfw/glfw/issues/1189
        // FIXME-VIEWPORT: Implement same work-around for Linux/OSX in the meanwhile.
        if (viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) {
            ::ShowWindow(hwnd, SW_SHOWNA);
            return;
        }
#endif
#endif

        glfwShowWindow(vd->window);
    }

    ImVec2 ImGuiImplGlfwGetWindowPos(ImGuiViewport* viewport) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        int                        x = 0, y = 0;
        glfwGetWindowPos(vd->window, &x, &y);
        return ImVec2((float)x, (float)y);
    }
    void ImGuiImplGlfwSetWindowPos(ImGuiViewport* viewport, ImVec2 pos) {
        ImGuiImplGlfwViewportData* vd     = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        vd->ignore_window_pos_event_frame = ImGui::GetFrameCount();
        glfwSetWindowPos(vd->window, (int)pos.x, (int)pos.y);
    }
    ImVec2 ImGuiImplGlfwGetWindowSize(ImGuiViewport* viewport) {

        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        int                        w = 0, h = 0;
        glfwGetWindowSize(vd->window, &w, &h);
        return ImVec2((float)w, (float)h);
    }

    void ImGuiImplGlfwSetWindowSize(ImGuiViewport* viewport, ImVec2 size) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
#if __APPLE__ && !GLFW_HAS_OSX_WINDOW_POS_FIX
        // Native OS windows are positioned from the bottom-left corner on macOS, whereas on other platforms they are
        // positioned from the upper-left corner. GLFW makes an effort to convert macOS style coordinates, however it
        // doesn't handle it when changing size. We are manually moving the window in order for changes of size to be based
        // on the upper-left corner.
        int x, y, width, height;
        glfwGetWindowPos(vd->Window, &x, &y);
        glfwGetWindowSize(vd->Window, &width, &height);
        glfwSetWindowPos(vd->Window, x, y - height + size.y);
#endif
        vd->ignore_window_size_event_frame = ImGui::GetFrameCount();
        glfwSetWindowSize(vd->window, (int)size.x, (int)size.y);
    }
    void ImGuiImplGlfwSetWindowTitle(ImGuiViewport* viewport, const char* title) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        glfwSetWindowTitle(vd->window, title);
    }
    void ImGuiImplGlfwSetWindowFocus(ImGuiViewport* viewport) {
#if GLFW_HAS_FOCUS_WINDOW
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        glfwFocusWindow(vd->Window);
#else
        // FIXME: What are the effect of not having this function? At the moment imgui doesn't actually call SetWindowFocus - we set that up ahead, will answer that question later.
        (void)viewport;
#endif
    }
    bool ImGuiImplGlfwGetWindowFocus(ImGuiViewport* viewport) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        return glfwGetWindowAttrib(vd->window, GLFW_FOCUSED) != 0;
    }
    bool ImGuiImplGlfwGetWindowMinimized(ImGuiViewport* viewport) {
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
        return glfwGetWindowAttrib(vd->window, GLFW_ICONIFIED) != 0;
    }
    void ImGuiImplGlfwRenderWindow(ImGuiViewport* viewport, void*) {
        ImGuiImplGlfwData*         bd = ImGuiImplGlfwGetBackendData();
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
    }
    void ImGuiImplGlfwSwapBuffers(ImGuiViewport* viewport, void*) {
        ImGuiImplGlfwData*         bd = ImGuiImplGlfwGetBackendData();
        ImGuiImplGlfwViewportData* vd = (ImGuiImplGlfwViewportData*)viewport->PlatformUserData;
    }

    void ImGuiImplGlfwRestoreCallbacks(GLFWwindow* window) {
        ImGuiImplGlfwData* bd = ImGuiImplGlfwGetBackendData();
        IM_ASSERT(bd->installed_callbacks == true && "Callbacks not installed!");
        IM_ASSERT(bd->window == window);

        glfwSetWindowFocusCallback(window, bd->prev_user_callback_window_focus);
        glfwSetCursorEnterCallback(window, bd->prev_user_callback_cursor_enter);
        glfwSetCursorPosCallback(window, bd->prev_user_callback_cursor_pos);
        glfwSetMouseButtonCallback(window, bd->prev_user_callback_mousebutton);
        glfwSetScrollCallback(window, bd->prev_user_callback_scroll);
        glfwSetKeyCallback(window, bd->prev_user_callback_key);
        glfwSetCharCallback(window, bd->prev_user_callback_char);
        glfwSetMonitorCallback(bd->prev_user_callback_monitor);
        bd->installed_callbacks             = false;
        bd->prev_user_callback_window_focus = nullptr;
        bd->prev_user_callback_cursor_enter = nullptr;
        bd->prev_user_callback_cursor_pos   = nullptr;
        bd->prev_user_callback_mousebutton  = nullptr;
        bd->prev_user_callback_scroll       = nullptr;
        bd->prev_user_callback_key          = nullptr;
        bd->prev_user_callback_char         = nullptr;
        bd->prev_user_callback_monitor      = nullptr;
    }
#pragma endregion
}// namespace Moer
