#ifndef MOER_WINDOW_CONTEXT_IMPLMENT_H
#define MOER_WINDOW_CONTEXT_IMPLMENT_H
#include "window/WindowContext.h"
namespace Moer {
class WindowImpl {
    friend class WindowContext;

public:
    virtual ~WindowImpl() {}
    virtual void PollEvents() const = 0;
    virtual void ShutDown() = 0;
    virtual void Tick()     = 0;

    //for multi-window support
    virtual void ApplyCursorMode(WindowHandle*, bool hidden) = 0;
    virtual void SetFocusMode(WindowHandle*, bool _focused)  = 0;
    virtual bool GetFocusMode(WindowHandle*) const            = 0;
    virtual Render::WindowFrameMetrics
    CaptureWindowFrameMetrics(const WindowHandle&) const      = 0;
    virtual void SetTitle(WindowHandle*, const char* _new_title) = 0;
    virtual void RequestClose(WindowHandle*)                     = 0;
    virtual bool ShouldClose(WindowHandle*) const                 = 0;
    virtual void ShowMainWindow()                                 = 0;

    virtual Render::SwapchainSurfaceInfo CreateSwapchainSurfaceInfo(const WindowHandle&) const = 0;

protected:
    static WindowImpl& GetInstance();
    virtual void       Init(const SurfaceInitInfo& info) = 0;

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
