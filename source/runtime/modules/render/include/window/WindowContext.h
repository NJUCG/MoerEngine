#ifndef MOERENGINE_WINDOW_CONTEXT_H
#define MOERENGINE_WINDOW_CONTEXT_H
#include "rhi/RHI.h"
#include <functional>
#include <vector>
namespace Moer {
    using WindowType = void;

    struct GuiWindowInitInfo {
        WindowType* window;
        bool        b_install_callbacks = true;
        ERHIType    rhi_type;
    };
    struct SurfaceInfo {
        SurfaceInfo(const char* _rhi_name,
                    uint32_t    _width,
                    uint32_t    _height,
                    const char* _title,
                    bool        _full_screen)
            : rhi_name(_rhi_name),
              width(_width),
              height(_height),
              title(_title),
              b_full_screen(_full_screen) {}
        const char* rhi_name;
        int         width{1280};
        int         height{720};
        const char* title{"Moer"};
        bool        b_full_screen{false};
    };
    class WindowContext {
        friend class WindowImpl;

    public:
        WindowContext() = default;
        ~WindowContext();
        static WindowContext& GetInstance();
        void                  Init(SurfaceInfo info);
        void                  Tick();
        void                  ShutDown();
        void                  GetWindowSize(int* width, int* height) const;
        void                  SetFocusMode(bool focused);
        bool                  GetFocusMode() const;
        WindowType*           GetWindow() const;
        void                  SetTitle(const char* newTitle);
        bool                  ShouldClose() const;
        void                  PollEvents() const;

        //for dx12 rhi
        void* GetNativeWindow() const;

        //for vulkan
        void CreateVulkanSurface(void* instance, WindowType* window, void* allocation_callback, void* surface);

        typedef std::function<void(unsigned int)>                                OnCharFunc;
        typedef std::function<void(int entered)>                                 OnCursorEnterFunc;
        typedef std::function<void(double xpos, double ypos)>                    OnCursorPosFunc;
        typedef std::function<void(int path_count, const char** paths)>          OnDropFunc;
        typedef std::function<void(int width, int height)>                       OnFrameBufferSizeFunc;
        typedef std::function<void(int key, int scancode, int action, int mods)> OnKeyFunc;
        typedef std::function<void(int button, int action, int mode)>            OnMouseButtonFunc;
        typedef std::function<void(double xoffset, double yoffset)>              OnScrollFunc;
        typedef std::function<void()>                                            OnWindowCloseFunc;
        typedef std::function<void(float xscale, float yscale)>                  OnWindowContentScaleFunc;
        typedef std::function<void(int xpos, int ypos)>                          OnWindowPosFunc;
        typedef std::function<void(int width, int height)>                       OnWindowSizeFunc;
        typedef std::function<void(int focused)>                                 OnWindowFocusFunc;

        inline void RegisterOnCharFunc(OnCharFunc func) {
            on_char_func.push_back(func);
        }
        inline void RegisterOnCursorEnterFunc(OnCursorEnterFunc func) {
            on_cursor_enter_func.push_back(func);
        }
        inline void RegisterOnCursorPosFunc(OnCursorPosFunc func) {
            on_cursor_pos_func.push_back(func);
        }
        inline void RegisterOnDropFunc(OnDropFunc func) {
            on_drop_func.push_back(func);
        }
        inline void RegisterOnFrameBufferSizeFunc(OnFrameBufferSizeFunc func) {
            on_frame_buffer_size_func.push_back(func);
        }
        inline void RegisterOnKeyFunc(OnKeyFunc func) {
            on_key_func.push_back(func);
        }
        inline void RegisterOnMouseButtonFunc(OnMouseButtonFunc func) {
            on_mouse_button_func.push_back(func);
        }
        inline void RegisterOnScrollFunc(OnScrollFunc func) {
            on_scroll_func.push_back(func);
        }
        inline void RegisterOnWindowCloseFunc(OnWindowCloseFunc func) {
            on_window_close_func.push_back(func);
        }
        inline void RegisterOnWindowContentScaleFunc(OnWindowContentScaleFunc func) {
            on_window_content_scale_func.push_back(func);
        }
        inline void RegisterOnWindowPosFunc(OnWindowPosFunc func) {
            on_window_pos_func.push_back(func);
        }
        inline void RegisterOnWindowSizeFunc(OnWindowSizeFunc func) {
            on_window_size_func.push_back(func);
        }
        inline void RegisterOnWindowFocusFunc(OnWindowFocusFunc func) {
            on_window_focus_func.push_back(func);
        }
        static void OnCharCallback(WindowType* window, unsigned int codepoint);
        static void OnCursorEnterCallback(WindowType* window, int entered);
        static void OnCursorPosCallback(WindowType* window, double xpos, double ypos);
        static void OnDropCallback(WindowType* window, int path_count, const char** paths);
        static void OnFramebufferSizeCallback(WindowType* window, int width, int height);
        static void OnKeyCallback(WindowType* window, int key, int scancode, int action, int mods);
        static void OnMouseButtonCallback(WindowType* window, int button, int action, int mode);
        static void OnScrollCallback(WindowType* window, double xoffset, double yoffset);
        static void OnWindowCloseCallback(WindowType* window);
        static void OnWindowContentScaleCallback(WindowType* window, float xscale, float yscale);
        static void OnWindowPosCallback(WindowType* window, int xpos, int ypos);
        static void OnWindowSizeCallback(WindowType* window, int width, int height);
        static void OnWindowFocusCallback(WindowType* window, int focused);

    protected:
        friend class GLFWWindowImpl;
        inline void OnChar(unsigned int codepoint) {
            for (auto func : on_char_func) {
                func(codepoint);
            }
        };
        inline void OnCursorEnter(int entered) {
            for (auto func : on_cursor_enter_func) {
                func(entered);
            }
        }
        inline void OnCursorPos(double xpos, double ypos) {
            for (auto func : on_cursor_pos_func) {
                func(xpos, ypos);
            }
        }
        inline void OnDrop(int path_count, const char** paths) {
            for (auto func : on_drop_func) {
                func(path_count, paths);
            }
        }
        inline void OnFramebufferSize(int width, int height) {
            for (auto func : on_frame_buffer_size_func) {
                func(width, height);
            }
        }
        inline void OnKey(int key, int scancode, int action, int mods) {
            for (auto func : on_key_func) {
                func(key, scancode, action, mods);
            }
        }
        inline void OnMouseButton(int button, int action, int mode) {
            for (auto func : on_mouse_button_func) {
                func(button, action, mode);
            }
        }
        inline void OnScroll(double xoffset, double yoffset) {
            for (auto func : on_scroll_func) {
                func(xoffset, yoffset);
            }
        }
        inline void OnWindowClose() {
            for (auto func : on_window_close_func) {
                func();
            }
        }
        inline void OnWindowContentScale(float xscale, float yscale) {
            for (auto func : on_window_content_scale_func) {
                func(xscale, yscale);
            }
        }
        inline void OnWindowPos(int xpos, int ypos) {
            for (auto func : on_window_pos_func) {
                func(xpos, ypos);
            }
        }
        inline void OnWindowSize(int width, int height) {
            for (auto func : on_window_size_func) {
                func(width, height);
            }
        }
        inline void OnWindowFocus(int focused) {
            for (auto func : on_window_focus_func) {
                func(focused);
            }
        }

    private:
        std::vector<OnCharFunc>               on_char_func;
        std::vector<OnCursorEnterFunc>        on_cursor_enter_func;
        std::vector<OnCursorPosFunc>          on_cursor_pos_func;
        std::vector<OnDropFunc>               on_drop_func;
        std::vector<OnFrameBufferSizeFunc>    on_frame_buffer_size_func;
        std::vector<OnKeyFunc>                on_key_func;
        std::vector<OnMouseButtonFunc>        on_mouse_button_func;
        std::vector<OnScrollFunc>             on_scroll_func;
        std::vector<OnWindowCloseFunc>        on_window_close_func;
        std::vector<OnWindowContentScaleFunc> on_window_content_scale_func;
        std::vector<OnWindowPosFunc>          on_window_pos_func;
        std::vector<OnWindowSizeFunc>         on_window_size_func;
        std::vector<OnWindowFocusFunc>        on_window_focus_func;
    };
}// namespace Moer
#endif// !UICONTEXT_H