#ifndef MOER_GLFW_UI_IMPL_H
#define MOER_GLFW_UI_IMPL_H

#include "rhi/RHI.h"
#include "window/WindowContext.h"

#include <imgui.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#undef APIENTRY
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>// for glfwGetWin32Window()
#endif
namespace Moer {
    struct ImGuiImplGlfwData {
        GLFWwindow* window;
        ERHIType    client_api;
        double      time;
        GLFWwindow* mouse_window;
        GLFWcursor* mouse_cursors[ImGuiMouseCursor_COUNT];
        ImVec2      last_valid_mouse_pos;
        GLFWwindow* key_owner_windows[GLFW_KEY_LAST];
        bool        installed_callbacks;
        bool        callbacks_chain_for_all_windows;
        bool        want_update_monitors;

        // Chain GLFW callbacks: our callbacks will call the user's previously installed callbacks, if any.
        GLFWwindowfocusfun prev_user_callback_window_focus;
        GLFWcursorposfun   prev_user_callback_cursor_pos;
        GLFWcursorenterfun prev_user_callback_cursor_enter;
        GLFWmousebuttonfun prev_user_callback_mousebutton;
        GLFWscrollfun      prev_user_callback_scroll;
        GLFWkeyfun         prev_user_callback_key;
        GLFWcharfun        prev_user_callback_char;
        GLFWmonitorfun     prev_user_callback_monitor;
#ifdef _WIN32
        WNDPROC glfw_wnd_proc;
#endif

        ImGuiImplGlfwData() { memset((void*)this, 0, sizeof(*this)); }
    };
    struct ImGuiImplGlfwViewportData {
        GLFWwindow* window;
        bool        window_owned;
        int         ignore_window_pos_event_frame;
        int         ignore_window_size_event_frame;

        ImGuiImplGlfwViewportData() {
            window                         = nullptr;
            window_owned                   = false;
            ignore_window_size_event_frame = ignore_window_pos_event_frame = -1;
        }
        ~ImGuiImplGlfwViewportData() { IM_ASSERT(window == nullptr); }
    };

    //register glfw window functions to imgui platform functions interface
    void GuiWindowInit(const GuiWindowInitInfo&);
    void GuiWindowShutDown();

    //update gui io and display size
    void GuiWindowNewFrame();
}// namespace Moer

#endif