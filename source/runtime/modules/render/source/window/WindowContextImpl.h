#ifndef MOER_WINDOW_CONTEXT_IMPLMENT_H
#define MOER_WINDOW_CONTEXT_IMPLMENT_H
#include "window/WindowContext.h"
namespace Moer {
    class WindowImpl {
        friend class WindowContext;

    public:
        virtual ~WindowImpl() {}
        // virtual void  SetFocusMode(bool _focused)                          = 0;
        // virtual void  GetWindowSize(int32_t* width, int32_t* height) const = 0;
        // virtual void  SetTitle(const char* _new_title)                     = 0;
        // virtual bool  ShouldClose() const                                  = 0;
        virtual void PollEvents() const = 0;
        // virtual void* GetNativeWindow() const                              = 0;
        virtual void ShutDown() = 0;
        virtual void Tick()     = 0;

        //for multi-window support
        virtual void SetFocusMode(WindowHandle*, bool _focused)                          = 0;
        virtual void GetWindowSize(WindowHandle*, int32_t* width, int32_t* height) const = 0;
        virtual void SetTitle(WindowHandle*, const char* _new_title)                     = 0;
        virtual bool ShouldClose(WindowHandle*) const                                    = 0;

        virtual void* GetNativeWindow(WindowHandle*) const = 0;

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
        static WindowImpl& GetInstance();
        virtual void       Init(const SurfaceInitInfo& info)                                                                   = 0;
        virtual void       OnCharCallbackImpl(WindowType* window, unsigned int codepoint)                                      = 0;
        virtual void       OnCursorEnterCallbackImpl(WindowType* window, int entered)                                          = 0;
        virtual void       OnCursorPosCallbackImpl(WindowType* window, double xpos, double ypos)                               = 0;
        virtual void       OnDropCallbackImpl(WindowType* window, int path_count, const char** paths)                          = 0;
        virtual void       OnFramebufferSizeCallbackImpl(WindowType* window, int width, int height)                            = 0;
        virtual void       OnKeyCallbackImpl(WindowType* window, int key, int scancode, int action, int mods)                  = 0;
        virtual void       OnMouseButtonCallbackImpl(WindowType* window, int button, int action, int mode)                     = 0;
        virtual void       OnScrollCallbackImpl(WindowType* window, double xoffset, double yoffset)                            = 0;
        virtual void       OnWindowCloseCallbackImpl(WindowType* window)                                                       = 0;
        virtual void       OnWindowContentScaleCallbackImpl(WindowType* window, float xscale, float yscale)                    = 0;
        virtual void       OnWindowPosCallbackImpl(WindowType* window, int xpos, int ypos)                                     = 0;
        virtual void       OnWindowSizeCallbackImpl(WindowType* window, int width, int height)                                 = 0;
        virtual void       OnWindowFocusCallbackImpl(WindowType* window, int focused)                                          = 0;
        virtual void       CreateVulkanSurface(void* instance, WindowHandle* window, void* allocation_callback, void* surface) = 0;

        void OnChar(WindowType* _type, unsigned int codepoint);
        void OnCursorEnter(WindowType* _type, int entered);
        void OnCursorPos(WindowType* _type, double xpos, double ypos);
        void OnDrop(WindowType* _type, int path_count, const char** paths);
        void OnFramebufferSize(WindowType* _type, int width, int height);
        void OnKey(WindowType* _type, int key, int scancode, int action, int mods);
        void OnMouseButton(WindowType* _type, int button, int action, int mode);
        void OnScroll(WindowType* _type, double xoffset, double yoffset);
        void OnWindowClose(WindowType* _type);
        void OnWindowContentScale(WindowType* _type, float xscale, float yscale);
        void OnWindowPos(WindowType* _type, int xpos, int ypos);
        void OnWindowSize(WindowType* _type, int width, int height);
        void OnWindowFocus(WindowType* _type, int focused);

        void RegisterOnCharFunc(WindowType* handle, WindowContext::OnCharFunc func);
        void RegisterOnCursorEnterFunc(WindowType* handle, WindowContext::OnCursorEnterFunc func);
        void RegisterOnCursorPosFunc(WindowType* handle, WindowContext::OnCursorPosFunc func);
        void RegisterOnDropFunc(WindowType* handle, WindowContext::OnDropFunc func);
        void RegisterOnFrameBufferSizeFunc(WindowType* handle, WindowContext::OnFrameBufferSizeFunc func);
        void RegisterOnKeyFunc(WindowType* handle, WindowContext::OnKeyFunc func);
        void RegisterOnMouseButtonFunc(WindowType* handle, WindowContext::OnMouseButtonFunc func);
        void RegisterOnScrollFunc(WindowType* handle, WindowContext::OnScrollFunc func);
        void RegisterOnWindowCloseFunc(WindowType* handle, WindowContext::OnWindowCloseFunc func);
        void RegisterOnWindowContentScaleFunc(WindowType* handle, WindowContext::OnWindowContentScaleFunc func);
        void RegisterOnWindowPosFunc(WindowType* handle, WindowContext::OnWindowPosFunc func);
        void RegisterOnWindowSizeFunc(WindowType* handle, WindowContext::OnWindowSizeFunc func);
        void RegisterOnWindowFocusFunc(WindowType* handle, WindowContext::OnWindowFocusFunc func);

    protected:
        WindowImpl() {}
        WindowHandle main_window_handle;
        // WindowType* window;
        int  width{0};
        int  height{0};
        bool focused{false};

    private:
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnCharFunc>>               window_on_char_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnCursorEnterFunc>>        window_on_cursor_enter_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnCursorPosFunc>>          window_on_cursor_pos_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnDropFunc>>               window_on_drop_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnFrameBufferSizeFunc>>    window_on_frame_buffer_size_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnKeyFunc>>                window_on_key_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnMouseButtonFunc>>        window_on_mouse_button_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnScrollFunc>>             window_on_scroll_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnWindowCloseFunc>>        window_on_window_close_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnWindowContentScaleFunc>> window_on_window_content_scale_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnWindowPosFunc>>          window_on_window_pos_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnWindowSizeFunc>>         window_on_window_size_func;
        Moer::UnorderedMap<WindowType*, Moer::Array<WindowContext::OnWindowFocusFunc>>        window_on_window_focus_func;
    };
}// namespace Moer
#endif
