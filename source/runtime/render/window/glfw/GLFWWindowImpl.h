#ifndef MOER_GLFW_CONTEXT_H
#define MOER_GLFW_CONTEXT_H

#include "../WindowContextImpl.h"
#include "window/WindowContext.h"

namespace Moer {
class GLFWWindowImpl final : public WindowImpl {
    friend WindowImpl;

public:
    virtual ~GLFWWindowImpl();

    virtual void PollEvents() const override;
    virtual void Tick() override;
    virtual void ShutDown() override;

    //for multi-window support
    virtual void  SetFocusMode(WindowHandle*, bool _focused) override;
    virtual void  GetWindowSize(WindowHandle*, int32_t* width, int32_t* height) const override;
    virtual void  SetTitle(WindowHandle*, const char* _new_title) override;
    virtual void  RequestClose(WindowHandle*) override;
    virtual bool  ShouldClose(WindowHandle*) const override;
    virtual Render::SwapchainSurfaceInfo CreateSwapchainSurfaceInfo(const WindowHandle&) const override;
    virtual void* GetInteropHandle(const WindowHandle*, EWindowInteropHandleType type) const override;

private:
    void TickCursorState();
    void SetCursorHide();
    void SetCursorNormal();

    GLFWWindowImpl();
    virtual void Init(const SurfaceInitInfo&) override;
};
} // namespace Moer
#endif
