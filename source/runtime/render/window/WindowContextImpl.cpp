#include "WindowContextImpl.h"
#include "window/WindowContext.h"
#define WINDOW_USE_GLFW
#if defined(WINDOW_USE_GLFW)
#include "glfw/GLFWWindowImpl.h"
#endif

namespace Moer {
    WindowImpl& WindowImpl::GetInstance() {
#if defined(WINDOW_USE_GLFW)
        static GLFWWindowImpl impl;
#endif
        return impl;
    };

    void WindowImpl::OnCharCallback(WindowType* window, unsigned int codepoint) {
        WindowImpl::GetInstance().OnCharCallbackImpl(window, codepoint);
    }
    void WindowImpl::OnCursorEnterCallback(WindowType* window, int entered) {
        WindowImpl::GetInstance().OnCursorEnterCallbackImpl(window, entered);
    }
    void WindowImpl::OnCursorPosCallback(WindowType* window, double xpos, double ypos) {
        WindowImpl::GetInstance().OnCursorPosCallbackImpl(window, xpos, ypos);
    }
    void WindowImpl::OnDropCallback(WindowType* window, int path_count, const char** paths) {
        WindowImpl::GetInstance().OnDropCallbackImpl(window, path_count, paths);
    }
    void WindowImpl::OnFramebufferSizeCallback(WindowType* window, int width, int height) {
        WindowImpl::GetInstance().OnFramebufferSizeCallbackImpl(window, width, height);
    }
    void WindowImpl::OnKeyCallback(WindowType* window, int key, int scancode, int action, int mods) {
        WindowImpl::GetInstance().OnKeyCallbackImpl(window, key, scancode, action, mods);
    }
    void WindowImpl::OnMouseButtonCallback(WindowType* window, int button, int action, int mode) {
        WindowImpl::GetInstance().OnMouseButtonCallbackImpl(window, button, action, mode);
    }
    void WindowImpl::OnScrollCallback(WindowType* window, double xoffset, double yoffset) {
        WindowImpl::GetInstance().OnScrollCallbackImpl(window, xoffset, yoffset);
    }
    void WindowImpl::OnWindowCloseCallback(WindowType* window) {
        WindowImpl::GetInstance().OnWindowCloseCallbackImpl(window);
    }
    void WindowImpl::OnWindowContentScaleCallback(WindowType* window, float xscale, float yscale) {
        WindowImpl::GetInstance().OnWindowContentScaleCallbackImpl(window, xscale, yscale);
    }
    void WindowImpl::OnWindowPosCallback(WindowType* window, int xpos, int ypos) {
        WindowImpl::GetInstance().OnWindowPosCallbackImpl(window, xpos, ypos);
    }
    void WindowImpl::OnWindowSizeCallback(WindowType* window, int width, int height) {
        WindowImpl::GetInstance().OnWindowSizeCallbackImpl(window, width, height);
    }
    void WindowImpl::OnWindowFocusCallback(WindowType* window, int focused) {
        WindowImpl::GetInstance().OnWindowFocusCallbackImpl(window, focused);
    }

    void WindowImpl::RegisterOnCharFunc(WindowType* handle, WindowContext::OnCharFunc func) {
        auto& handle_vector = window_on_char_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnCursorEnterFunc(WindowType* handle, WindowContext::OnCursorEnterFunc func) {
        auto& handle_vector = window_on_cursor_enter_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnCursorPosFunc(WindowType* handle, WindowContext::OnCursorPosFunc func) {
        auto& handle_vector = window_on_cursor_pos_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnDropFunc(WindowType* handle, WindowContext::OnDropFunc func) {
        auto& handle_vector = window_on_drop_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnFrameBufferSizeFunc(WindowType* handle, WindowContext::OnFrameBufferSizeFunc func) {
        auto& handle_vector = window_on_frame_buffer_size_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnKeyFunc(WindowType* handle, WindowContext::OnKeyFunc func) {
        auto& handle_vector = window_on_key_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnMouseButtonFunc(WindowType* handle, WindowContext::OnMouseButtonFunc func) {
        auto& handle_vector = window_on_mouse_button_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnScrollFunc(WindowType* handle, WindowContext::OnScrollFunc func) {
        auto& handle_vector = window_on_scroll_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnWindowCloseFunc(WindowType* handle, WindowContext::OnWindowCloseFunc func) {
        auto& handle_vector = window_on_window_close_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnWindowContentScaleFunc(WindowType* handle, WindowContext::OnWindowContentScaleFunc func) {
        auto& handle_vector = window_on_window_content_scale_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnWindowPosFunc(WindowType* handle, WindowContext::OnWindowPosFunc func) {
        auto& handle_vector = window_on_window_pos_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnWindowSizeFunc(WindowType* handle, WindowContext::OnWindowSizeFunc func) {
        auto& handle_vector = window_on_window_size_func[handle];
        handle_vector.push_back(func);
    }
    void WindowImpl::RegisterOnWindowFocusFunc(WindowType* handle, WindowContext::OnWindowFocusFunc func) {
        auto& handle_vector = window_on_window_focus_func[handle];
        handle_vector.push_back(func);
    }

    void WindowImpl::OnChar(WindowType* _type, unsigned int codepoint) {
        auto& handle_vector = window_on_char_func[_type];
        for (auto func : handle_vector) {
            func(codepoint);
        }
    };
    void WindowImpl::OnCursorEnter(WindowType* _type, int entered) {
        auto& handle_vector = window_on_cursor_enter_func[_type];
        for (auto func : handle_vector) {
            func(entered);
        }
    }
    void WindowImpl::OnCursorPos(WindowType* _type, double xpos, double ypos) {
        auto& handle_vector = window_on_cursor_pos_func[_type];
        for (auto func : handle_vector) {
            func(xpos, ypos);
        }
    }
    void WindowImpl::OnDrop(WindowType* _type, int path_count, const char** paths) {
        auto& handle_vector = window_on_drop_func[_type];
        for (auto func : handle_vector) {
            func(path_count, paths);
        }
    }
    void WindowImpl::OnFramebufferSize(WindowType* _type, int width, int height) {
        auto& handle_vector = window_on_frame_buffer_size_func[_type];
        for (auto func : handle_vector) {
            func(width, height);
        }
    }
    void WindowImpl::OnKey(WindowType* _type, int key, int scancode, int action, int mods) {
        auto& handle_vector = window_on_key_func[_type];
        for (auto func : handle_vector) {
            func(key, scancode, action, mods);
        }
    }
    void WindowImpl::OnMouseButton(WindowType* _type, int button, int action, int mode) {
        auto& handle_vector = window_on_mouse_button_func[_type];
        for (auto func : handle_vector) {
            func(button, action, mode);
        }
    }
    void WindowImpl::OnScroll(WindowType* _type, double xoffset, double yoffset) {
        auto& handle_vector = window_on_scroll_func[_type];
        for (auto func : handle_vector) {
            func(xoffset, yoffset);
        }
    }
    void WindowImpl::OnWindowClose(WindowType* _type) {
        auto& handle_vector = window_on_window_close_func[_type];
        for (auto func : handle_vector) {
            func();
        }
    }
    void WindowImpl::OnWindowContentScale(WindowType* _type, float xscale, float yscale) {
        auto& handle_vector = window_on_window_content_scale_func[_type];
        for (auto func : handle_vector) {
            func(xscale, yscale);
        }
    }
    void WindowImpl::OnWindowPos(WindowType* _type, int xpos, int ypos) {
        auto& handle_vector = window_on_window_pos_func[_type];
        for (auto func : handle_vector) {
            func(xpos, ypos);
        }
    }
    void WindowImpl::OnWindowSize(WindowType* _type, int width, int height) {
        auto& handle_vector = window_on_window_size_func[_type];
        for (auto func : handle_vector) {
            func(width, height);
        }
    }
    void WindowImpl::OnWindowFocus(WindowType* _type, int focused) {
        auto& handle_vector = window_on_window_focus_func[_type];
        for (auto func : handle_vector) {
            func(focused);
        }
    }
}// namespace Moer