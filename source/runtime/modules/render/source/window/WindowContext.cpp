#include "window/WindowContext.h"
#include "WindowContextImpl.h"
#define GLFW_WINDOW

namespace Moer {

    WindowContext& WindowContext::GetInstance() {
        static WindowContext context;
        return context;
    }
    void WindowContext::Init(SurfaceInfo info) {
        WindowImpl::GetInstance().Init(info);
    }
    WindowContext::~WindowContext() {
    }
    WindowType* WindowContext::GetWindow() const {
        return WindowImpl::GetInstance().window;
    }
    void* WindowContext::GetNativeWindow() const {
        return WindowImpl::GetInstance().GetNativeWindow();
    }
    void WindowContext::GuiInit(const GuiWindowInitInfo& _init_info) {
        WindowImpl::GetInstance().GuiInit(_init_info);
    };
    void WindowContext::GuiUpdate() {
        WindowImpl::GetInstance().GuiUpdate();
    }

    bool WindowContext::GetFocusMode() const {
        return WindowImpl::GetInstance().focused;
    }
    void WindowContext::GetWindowSize(int* width, int* height) const {
        WindowImpl::GetInstance().GetWindowSize(width, height);
    }
    void WindowContext::SetFocusMode(bool focused) {
        WindowImpl::GetInstance().SetFocusMode(focused);
    };
    void WindowContext::SetTitle(const char* newTitle) {
        WindowImpl::GetInstance().SetTitle(newTitle);
    };
    bool WindowContext::ShouldClose() const {
        return WindowImpl::GetInstance().ShouldClose();
    };
    void WindowContext::PollEvents() const {
        WindowImpl::GetInstance().PollEvents();
    };

    void WindowContext::OnCharCallback(WindowType* window, unsigned int codepoint) {
        WindowImpl::GetInstance().OnCharCallbackImpl(window, codepoint);
    }
    void WindowContext::OnCursorEnterCallback(WindowType* window, int entered) {
        WindowImpl::GetInstance().OnCursorEnterCallbackImpl(window, entered);
    }
    void WindowContext::OnCursorPosCallback(WindowType* window, double xpos, double ypos) {
        WindowImpl::GetInstance().OnCursorPosCallbackImpl(window, xpos, ypos);
    }
    void WindowContext::OnDropCallback(WindowType* window, int path_count, const char** paths) {
        WindowImpl::GetInstance().OnDropCallbackImpl(window, path_count, paths);
    }
    void WindowContext::OnFramebufferSizeCallback(WindowType* window, int width, int height) {
        WindowImpl::GetInstance().OnFramebufferSizeCallbackImpl(window, width, height);
    }
    void WindowContext::OnKeyCallback(WindowType* window, int key, int scancode, int action, int mods) {
        WindowImpl::GetInstance().OnKeyCallbackImpl(window, key, scancode, action, mods);
    }
    void WindowContext::OnMouseButtonCallback(WindowType* window, int button, int action, int mode) {
        WindowImpl::GetInstance().OnMouseButtonCallbackImpl(window, button, action, mode);
    }
    void WindowContext::OnScrollCallback(WindowType* window, double xoffset, double yoffset) {
        WindowImpl::GetInstance().OnScrollCallbackImpl(window, xoffset, yoffset);
    }
    void WindowContext::OnWindowCloseCallback(WindowType* window) {
        WindowImpl::GetInstance().OnWindowCloseCallbackImpl(window);
    }
    void WindowContext::OnWindowContentScaleCallback(WindowType* window, float xscale, float yscale) {
        WindowImpl::GetInstance().OnWindowContentScaleCallbackImpl(window, xscale, yscale);
    }
    void WindowContext::OnWindowPosCallback(WindowType* window, int xpos, int ypos) {
        WindowImpl::GetInstance().OnWindowPosCallbackImpl(window, xpos, ypos);
    }
    void WindowContext::OnWindowSizeCallback(WindowType* window, int width, int height) {
        WindowImpl::GetInstance().OnWindowSizeCallbackImpl(window, width, height);
    }
    void WindowContext::OnWindowFocusCallback(WindowType* window, int focused) {
        WindowImpl::GetInstance().OnWindowFocusCallbackImpl(window, focused);
    }
    void WindowContext::CreateVulkanSurface(void* instance, WindowType* window, void* allocation_callback, void* surface) {
        WindowImpl::GetInstance().CreateVulkanSurface(instance, window, allocation_callback, surface);
    }
}// namespace Moer