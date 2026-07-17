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

void WindowContext::RequestClose(WindowHandle* window) {
    WindowImpl::GetInstance().RequestClose(window);
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

void WindowContext::ShowMainWindow() {
    WindowImpl::GetInstance().ShowMainWindow();
}

void WindowContext::CreateVulkanSurface(
    void*         instance,
    WindowHandle* window,
    void*         allocation_callback,
    void*         surface
) {
    WindowImpl::GetInstance().CreateVulkanSurface(instance, window, allocation_callback, surface);
};

void WindowContext::RegisterOnCharFunc(WindowType* handle, OnCharFunc func) {
    WindowImpl::GetInstance().RegisterOnCharFunc(handle, func);
}
void WindowContext::RegisterOnCursorEnterFunc(WindowType* handle, OnCursorEnterFunc func) {
    WindowImpl::GetInstance().RegisterOnCursorEnterFunc(handle, func);
}
void WindowContext::RegisterOnCursorPosFunc(WindowType* handle, OnCursorPosFunc func) {
    WindowImpl::GetInstance().RegisterOnCursorPosFunc(handle, func);
}
void WindowContext::RegisterOnDropFunc(WindowType* handle, OnDropFunc func) {
    WindowImpl::GetInstance().RegisterOnDropFunc(handle, func);
}
void WindowContext::RegisterOnFrameBufferSizeFunc(WindowType* handle, OnFrameBufferSizeFunc func) {
    WindowImpl::GetInstance().RegisterOnFrameBufferSizeFunc(handle, func);
}
void WindowContext::RegisterOnKeyFunc(WindowType* handle, OnKeyFunc func) {
    WindowImpl::GetInstance().RegisterOnKeyFunc(handle, func);
}
void WindowContext::RegisterOnMouseButtonFunc(WindowType* handle, OnMouseButtonFunc func) {
    WindowImpl::GetInstance().RegisterOnMouseButtonFunc(handle, func);
}
void WindowContext::RegisterOnScrollFunc(WindowType* handle, OnScrollFunc func) {
    WindowImpl::GetInstance().RegisterOnScrollFunc(handle, func);
}
void WindowContext::RegisterOnWindowCloseFunc(WindowType* handle, OnWindowCloseFunc func) {
    WindowImpl::GetInstance().RegisterOnWindowCloseFunc(handle, func);
}
void WindowContext::RegisterOnWindowContentScaleFunc(WindowType* handle, OnWindowContentScaleFunc func) {
    WindowImpl::GetInstance().RegisterOnWindowContentScaleFunc(handle, func);
}
void WindowContext::RegisterOnWindowPosFunc(WindowType* handle, OnWindowPosFunc func) {
    WindowImpl::GetInstance().RegisterOnWindowPosFunc(handle, func);
}
void WindowContext::RegisterOnWindowSizeFunc(WindowType* handle, OnWindowSizeFunc func) {
    WindowImpl::GetInstance().RegisterOnWindowSizeFunc(handle, func);
}
void WindowContext::RegisterOnWindowFocusFunc(WindowType* handle, OnWindowFocusFunc func) {
    WindowImpl::GetInstance().RegisterOnWindowFocusFunc(handle, func);
}

} // namespace Moer
