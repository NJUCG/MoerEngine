#include "window/WindowContext.h"
#include "WindowContextImpl.h"
#define GLFW_WINDOW

namespace Moer {

void WindowContext::Init(const SurfaceInitInfo& info) {
    WindowImpl::GetInstance().Init(info);
}
void WindowContext::Tick() {
    WindowImpl::GetInstance().Tick();
}
void WindowContext::ShutDown() {
    WindowImpl::GetInstance().ShutDown();
}
WindowContext::~WindowContext() {}

void WindowContext::ApplyCursorMode(WindowHandle* window, bool hidden) {
    WindowImpl::GetInstance().ApplyCursorMode(window, hidden);
}

void WindowContext::SetFocusMode(WindowHandle* window, bool focused) {
    WindowImpl::GetInstance().SetFocusMode(window, focused);
};

bool WindowContext::GetFocusMode(WindowHandle* window) {
    return WindowImpl::GetInstance().GetFocusMode(window);
}

Render::WindowFrameMetrics WindowContext::CaptureWindowFrameMetrics(const WindowHandle& window) {
    return WindowImpl::GetInstance().CaptureWindowFrameMetrics(window);
}

void WindowContext::SetTitle(WindowHandle* window, const char* newTitle) {
    WindowImpl::GetInstance().SetTitle(window, newTitle);
};

void WindowContext::RequestClose(WindowHandle* window) {
    WindowImpl::GetInstance().RequestClose(window);
};

bool WindowContext::ShouldClose(WindowHandle* window) {
    return WindowImpl::GetInstance().ShouldClose(window);
};

WindowHandle* WindowContext::GetMainWindow() {
    return &WindowImpl::GetInstance().main_window_handle;
};

void WindowContext::ShowMainWindow() {
    WindowImpl::GetInstance().ShowMainWindow();
}

Render::SwapchainSurfaceInfo WindowContext::CreateSwapchainSurfaceInfo(const WindowHandle& window) {
    return WindowImpl::GetInstance().CreateSwapchainSurfaceInfo(window);
}

} // namespace Moer
