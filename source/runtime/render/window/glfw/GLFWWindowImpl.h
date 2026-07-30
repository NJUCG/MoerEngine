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
    virtual void ApplyCursorMode(WindowHandle*, bool hidden) override;
    virtual void SetFocusMode(WindowHandle*, bool _focused) override;
    virtual bool GetFocusMode(WindowHandle*) const override;
    virtual Render::WindowFrameMetrics
    CaptureWindowFrameMetrics(const WindowHandle&) const override;
    virtual void SetTitle(WindowHandle*, const char* _new_title) override;
    virtual void RequestClose(WindowHandle*) override;
    virtual bool ShouldClose(WindowHandle*) const override;
    virtual void ShowMainWindow() override;
    virtual Render::SwapchainSurfaceInfo CreateSwapchainSurfaceInfo(const WindowHandle&) const override;

private:
    void SetCursorHide(WindowHandle*);
    void SetCursorNormal(WindowHandle*);

    GLFWWindowImpl();
    virtual void Init(const SurfaceInitInfo&) override;

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
