#ifndef MOER_WINDOW_CONTEXT_IMPLMENT_H
#define MOER_WINDOW_CONTEXT_IMPLMENT_H
#include "window/WindowContext.h"
namespace Moer {
    class WindowImpl {
        friend class WindowContext;

    public:
        virtual ~WindowImpl() {}
        virtual void  SetFocusMode(bool _focused)                          = 0;
        virtual void  GetWindowSize(int32_t* width, int32_t* height) const = 0;
        virtual void  SetTitle(const char* _new_title)                     = 0;
        virtual bool  ShouldClose() const                                  = 0;
        virtual void  PollEvents() const                                   = 0;
        virtual void* GetNativeWindow() const                              = 0;
        virtual void  ShutDown()                                           = 0;
        virtual void  Tick()                                               = 0;

    protected:
        static WindowImpl& GetInstance();
        virtual void       Init(const SurfaceInfo& info)                                                                     = 0;
        virtual void       OnCharCallbackImpl(WindowType* window, unsigned int codepoint)                                    = 0;
        virtual void       OnCursorEnterCallbackImpl(WindowType* window, int entered)                                        = 0;
        virtual void       OnCursorPosCallbackImpl(WindowType* window, double xpos, double ypos)                             = 0;
        virtual void       OnDropCallbackImpl(WindowType* window, int path_count, const char** paths)                        = 0;
        virtual void       OnFramebufferSizeCallbackImpl(WindowType* window, int width, int height)                          = 0;
        virtual void       OnKeyCallbackImpl(WindowType* window, int key, int scancode, int action, int mods)                = 0;
        virtual void       OnMouseButtonCallbackImpl(WindowType* window, int button, int action, int mode)                   = 0;
        virtual void       OnScrollCallbackImpl(WindowType* window, double xoffset, double yoffset)                          = 0;
        virtual void       OnWindowCloseCallbackImpl(WindowType* window)                                                     = 0;
        virtual void       OnWindowContentScaleCallbackImpl(WindowType* window, float xscale, float yscale)                  = 0;
        virtual void       OnWindowPosCallbackImpl(WindowType* window, int xpos, int ypos)                                   = 0;
        virtual void       OnWindowSizeCallbackImpl(WindowType* window, int width, int height)                               = 0;
        virtual void       OnWindowFocusCallbackImpl(WindowType* window, int focused)                                        = 0;
        virtual void       CreateVulkanSurface(void* instance, WindowType* window, void* allocation_callback, void* surface) = 0;

        virtual void GuiInit(const GuiWindowInitInfo&) = 0;
        virtual void GuiUpdate()                       = 0;
        virtual void GuiShutDown()                     = 0;

    protected:
        WindowImpl() {}
        WindowType* window;
        int         width{0};
        int         height{0};
        bool        focused{false};
    };
}// namespace Moer
#endif
