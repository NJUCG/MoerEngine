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

protected:
    static WindowImpl& GetInstance();
    virtual void       Init(const SurfaceInitInfo& info)                                                  = 0;
    virtual void
    CreateVulkanSurface(void* instance, WindowHandle* window, void* allocation_callback, void* surface) = 0;

protected:
    WindowImpl() {}
    WindowHandle main_window_handle;
    // WindowType* window;
    int  width{0};
    int  height{0};
    bool focused{false};
};
} // namespace Moer
#endif
