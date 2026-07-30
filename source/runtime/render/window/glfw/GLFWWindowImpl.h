#ifndef MOER_GLFW_CONTEXT_H
#define MOER_GLFW_CONTEXT_H

#include "../WindowContextImpl.h"
#include "window/WindowContext.h"

#include <atomic>
#include <mutex>

namespace Moer {
struct GLFWWindowSurfaceLeaseState;

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
    virtual void  ShowMainWindow() override;
    virtual Render::SwapchainSurfaceInfo CreateSwapchainSurfaceInfo(const WindowHandle&) const override;

private:
    void TickCursorState();
    void SetCursorHide();
    void SetCursorNormal();

    GLFWWindowImpl();
    virtual void Init(const SurfaceInitInfo&) override;
    virtual void OnCharCallbackImpl(WindowType* window, unsigned int codepoint) override;
    virtual void OnCursorEnterCallbackImpl(WindowType* window, int entered) override;
    virtual void OnCursorPosCallbackImpl(WindowType* window, double xpos, double ypos) override;
    virtual void OnDropCallbackImpl(WindowType* window, int path_count, const char** paths) override;
    virtual void OnFramebufferSizeCallbackImpl(WindowType* window, int width, int height) override;
    virtual void OnKeyCallbackImpl(WindowType* window, int key, int scancode, int action, int mods) override;
    virtual void OnMouseButtonCallbackImpl(WindowType* window, int button, int action, int mode) override;
    virtual void OnScrollCallbackImpl(WindowType* window, double xoffset, double yoffset) override;
    virtual void OnWindowCloseCallbackImpl(WindowType* window) override;
    virtual void OnWindowContentScaleCallbackImpl(WindowType* window, float xscale, float yscale) override;
    virtual void OnWindowPosCallbackImpl(WindowType* window, int xpos, int ypos) override;
    virtual void OnWindowSizeCallbackImpl(WindowType* window, int width, int height) override;
    virtual void OnWindowFocusCallbackImpl(WindowType* window, int focused) override;

    void InstallInterface(WindowHandle* _handle);

private:
    bool m_deferred_fullscreen        = false;
    int  m_deferred_fullscreen_width  = 0;
    int  m_deferred_fullscreen_height = 0;

    mutable std::mutex                                                                  surface_source_mutex;
    mutable UnorderedMap<WindowType*, std::weak_ptr<const Render::WindowSurfaceSource>> surface_sources;
    mutable std::atomic_uint64_t                                                        next_surface_generation{1};
    SharedPtr<GLFWWindowSurfaceLeaseState>                                              surface_lease_state;
};
} // namespace Moer
#endif
