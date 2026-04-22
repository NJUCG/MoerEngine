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

void WindowContext::SetFocusMode(WindowHandle* window, bool focused) {
    WindowImpl::GetInstance().SetFocusMode(window, focused);
};

void WindowContext::GetWindowSize(WindowHandle* window, int* width, int* height) {
    WindowImpl::GetInstance().GetWindowSize(window, width, height);
};

void WindowContext::SetTitle(WindowHandle* window, const char* newTitle) {
    WindowImpl::GetInstance().SetTitle(window, newTitle);
};

bool WindowContext::ShouldClose(WindowHandle* window) {
    return WindowImpl::GetInstance().ShouldClose(window);
};

void* WindowContext::GetNativeWindow(WindowHandle* window) {
    return WindowImpl::GetInstance().GetNativeWindow(window);
};

WindowHandle* WindowContext::GetMainWindow() {
    return &WindowImpl::GetInstance().main_window_handle;
};

void WindowContext::CreateVulkanSurface(
    void*         instance,
    WindowHandle* window,
    void*         allocation_callback,
    void*         surface
) {
    WindowImpl::GetInstance().CreateVulkanSurface(instance, window, allocation_callback, surface);
};

} // namespace Moer
