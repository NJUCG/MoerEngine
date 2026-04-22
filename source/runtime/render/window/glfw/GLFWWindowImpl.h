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
    virtual void
    CreateVulkanSurface(void* instance, WindowHandle* window, void* allocation_callback, void* surface)
        override;
    virtual void Tick() override;
    virtual void ShutDown() override;

    //for multi-window support
    virtual void  SetFocusMode(WindowHandle*, bool _focused) override;
    virtual void  GetWindowSize(WindowHandle*, int32_t* width, int32_t* height) const override;
    virtual void  SetTitle(WindowHandle*, const char* _new_title) override;
    virtual bool  ShouldClose(WindowHandle*) const override;
    virtual void* GetNativeWindow(WindowHandle*) const override;

private:
    void TickCursorState();
    void SetCursorHide();
    void SetCursorNormal();

    GLFWWindowImpl();
    virtual void Init(const SurfaceInitInfo&) override;

private:
    void InitVulkan();
    void InitD3D12();
};
} // namespace Moer
#endif
