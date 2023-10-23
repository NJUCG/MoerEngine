#include "GLFWWindowImpl.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "window/WindowContext.h"
#include "platform/Platform.h"
//define vulkan ahead of glfw
#include "vulkan/vulkan.h"
#include "GLFW/glfw3.h"
#if PLATFORM_WINDOWS
//for dx12
//https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "log/LogSystem.h"
#include <string.h>

namespace Moer {
    GLFWWindowImpl::GLFWWindowImpl() {
    }
    GLFWWindowImpl::~GLFWWindowImpl() {
        glfwDestroyWindow((GLFWwindow*)window);
    }

    void  GLFWWindowImpl::SetFocusMode(bool _focused) { glfwSetInputMode((GLFWwindow*)window, GLFW_CURSOR, focused ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL); }
    void  GLFWWindowImpl::GetWindowSize(int32_t* width, int32_t* height) const { glfwGetWindowSize((GLFWwindow*)window, width, height); }
    void  GLFWWindowImpl::SetTitle(const char* _new_title) { glfwSetWindowTitle((GLFWwindow*)window, _new_title); }
    bool  GLFWWindowImpl::ShouldClose() const { return glfwWindowShouldClose((GLFWwindow*)window); }
    void  GLFWWindowImpl::PollEvents() const { glfwPollEvents(); }
    void* GLFWWindowImpl::GetNativeWindow() const {
#if PLATFORM_WINDOWS
        return glfwGetWin32Window((GLFWwindow*)window);
#endif
        return nullptr;
    }

    void GLFWWindowImpl::Init(const SurfaceInfo& info) {
        if (!glfwInit()) {
            //error log and quit
            LOG_ERROR("Window init fail.");
            assert(0 && "Window init fail.");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        if (strcmp(info.rhi_name, "D3D12")) {
            InitD3D12();
        } else {
            InitVulkan();
        }

        GLFWwindow* window = glfwCreateWindow(info.width, info.height, info.title, nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);

        glfwSetCharCallback(window, [](GLFWwindow* window, unsigned int codepoint) { WindowContext::OnCharCallback((WindowType*)window, codepoint); });
        glfwSetCursorEnterCallback(window, [](GLFWwindow* window, int entered) { WindowContext::OnCursorEnterCallback(window, entered); });
        glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) { WindowContext::OnCursorPosCallback(window, xpos, ypos); });
        glfwSetDropCallback(window, [](GLFWwindow* window, int path_count, const char** paths) { WindowContext::OnDropCallback(window, path_count, paths); });
        glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowContext::OnFramebufferSizeCallback(window, width, height); });
        glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) { WindowContext::OnKeyCallback(window, key, scancode, action, mods); });
        glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mode) { WindowContext::OnMouseButtonCallback(window, button, action, mode); });
        glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) { WindowContext::OnScrollCallback(window, xoffset, yoffset); });
        glfwSetWindowCloseCallback(window, [](GLFWwindow* window) { WindowContext::OnWindowCloseCallback(window); });
        glfwSetWindowContentScaleCallback(window, [](GLFWwindow* window, float xscale, float yscale) { WindowContext::OnWindowContentScaleCallback(window, xscale, yscale); });
        glfwSetWindowPosCallback(window, [](GLFWwindow* window, int xpos, int ypos) { WindowContext::OnWindowPosCallback(window, xpos, ypos); });
        glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowContext::OnWindowSizeCallback(window, width, height); });
        glfwSetWindowFocusCallback(window, [](GLFWwindow* window, int focused) { WindowContext::OnWindowFocusCallback(window, focused); });
        this->window = window;
    }

    void GLFWWindowImpl::InitVulkan() {
        if (!glfwVulkanSupported()) {
            LOG_ERROR("Vulkan not supported by Winodw System");
            exit(-1);
        }
    }
    void GLFWWindowImpl::InitD3D12() {
#if PLATFORM_WINDOWS

#endif
    }
    void GLFWWindowImpl::CreateVulkanSurface(void* instance, WindowType* window, void* allocation_callback, void* surface) {
        glfwCreateWindowSurface((VkInstance)instance, (GLFWwindow*)window, (const VkAllocationCallbacks*)allocation_callback, (VkSurfaceKHR*)surface);
    }

    void GLFWWindowImpl::OnCursorEnterCallbackImpl(WindowType* window, int entered) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnCursorEnter(entered);
        }
    };

    void GLFWWindowImpl::OnCharCallbackImpl(WindowType* window, unsigned int codepoint) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnChar(codepoint);
        }
    }

    void GLFWWindowImpl::OnCursorPosCallbackImpl(WindowType* window, double xpos, double ypos) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnCursorPos(xpos, ypos);
        }
    }
    void GLFWWindowImpl::OnDropCallbackImpl(WindowType* window, int path_count, const char** paths) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnDrop(path_count, paths);
        }
    }
    void GLFWWindowImpl::OnFramebufferSizeCallbackImpl(WindowType* window, int width, int height) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnFramebufferSize(width, height);
        }
    }
    void GLFWWindowImpl::OnKeyCallbackImpl(WindowType* window, int key, int scancode, int action, int mods) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnKey(key, scancode, action, mods);
        }
    }
    void GLFWWindowImpl::OnMouseButtonCallbackImpl(WindowType* window, int button, int action, int mode) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnMouseButton(button, action, mode);
        }
    }
    void GLFWWindowImpl::OnScrollCallbackImpl(WindowType* window, double xoffset, double yoffset) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnScroll(xoffset, yoffset);
        }
    }
    void GLFWWindowImpl::OnWindowCloseCallbackImpl(WindowType* window) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnWindowClose();
        }
    }
    void GLFWWindowImpl::OnWindowContentScaleCallbackImpl(WindowType* window, float xscale, float yscale) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnWindowContentScale(xscale, yscale);
        }
    }
    void GLFWWindowImpl::OnWindowPosCallbackImpl(WindowType* window, int xpos, int ypos) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnWindowPos(xpos, ypos);
        }
    }
    void GLFWWindowImpl::OnWindowSizeCallbackImpl(WindowType* window, int width, int height) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnWindowSize(width, height);
        }
    }
    void GLFWWindowImpl::OnWindowFocusCallbackImpl(WindowType* window, int focused) {
        auto* context = reinterpret_cast<WindowContext*>(glfwGetWindowUserPointer((GLFWwindow*)window));
        if (context) {
            context->OnWindowFocus(focused);
        }
    }
}// namespace Moer
