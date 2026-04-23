#include "GLFWWindowImpl.h"

#include "Core.h"
#include "misc/MMemory.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "window/WindowContext.h"

#include "GLFW/glfw3.h"

#include "IconsFontAwesome6.h"
#include <fstream>
#if PLATFORM_WINDOWS
//for dx12
//https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "log/LogSystem.h"
#include <string.h>

#include "window/WindowInput.h"

namespace Moer {

GLFWWindowImpl::GLFWWindowImpl() {}
GLFWWindowImpl::~GLFWWindowImpl() {}
void GLFWWindowImpl::PollEvents() const {
    glfwPollEvents();
}

void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
    if (!glfwInit()) {
        //error log and quit
        LOG_ERROR("Window init fail.");
        MOER_ASSERT(false, "GLFW initialization failed");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    static_cast<void>(info.rhi_type);

    int          width   = info.width;
    int          height  = info.height;
    GLFWmonitor* monitor = nullptr;
    if (info.b_fullscreen) {
        // 全屏模式
        monitor                 = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        width                   = mode->width;
        height                  = mode->height;
    }

    GLFWwindow* window = glfwCreateWindow(width, height, info.title.c_str(), monitor, nullptr);

    glfwSetWindowUserPointer(window, this);

    this->main_window_handle.window = window;
}

void GLFWWindowImpl::Tick() {
    // per-frame time logic
    float current_frame_time           = static_cast<float>(glfwGetTime());
    WindowInput::Get().delta_time      = current_frame_time - WindowInput::Get().last_frame_time;
    WindowInput::Get().last_frame_time = current_frame_time;

    PollEvents();

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize((GLFWwindow*)main_window_handle.window, &framebuffer_width, &framebuffer_height);
    WindowInput::Get().width        = static_cast<float>(framebuffer_width);
    WindowInput::Get().height       = static_cast<float>(framebuffer_height);
    WindowInput::Get().aspect_ratio = framebuffer_height == 0 ? 0.0f :
                                      static_cast<float>(framebuffer_width) / static_cast<float>(framebuffer_height);

    TickCursorState();
}
void GLFWWindowImpl::ShutDown() {
    // GuiShutDown();
    glfwDestroyWindow((GLFWwindow*)main_window_handle.window);
}

void GLFWWindowImpl::TickCursorState() {
    if (WindowInput::Get().force_cursor_visible) {
        SetCursorNormal();
        return;
    }
    if (WindowInput::Get().force_cursor_hidden) {
        SetCursorHide();
        return;
    }

    // hide cursor when **left or right** mouse button is pressed
    bool b_should_hide = WindowInput::Get().mouse_button_state[MouseButtons::Left] ||
                         WindowInput::Get().mouse_button_state[MouseButtons::Right] ||
                         WindowInput::Get().mouse_button_state[MouseButtons::Middle];

    // is_active <=> cursor is hovering on the SceneColor window
    if (WindowInput::Get().is_active && b_should_hide) {
        SetCursorHide();
    } else {
        SetCursorNormal();
    }
}

void GLFWWindowImpl::SetCursorHide() {
    glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    WindowInput::Get().is_cursor_hiding = true;
}

void GLFWWindowImpl::SetCursorNormal() {
    glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    WindowInput::Get().is_cursor_hiding = false;
    WindowInput::Get().is_cursor_dirty  = true;
}

void GLFWWindowImpl::SetFocusMode(WindowHandle* _window, bool _focused) {
    glfwSetInputMode(
        (GLFWwindow*)_window->window, GLFW_CURSOR, _focused ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
    );
}

void GLFWWindowImpl::GetWindowSize(WindowHandle* _window, int32_t* width, int32_t* height) const {
    glfwGetWindowSize((GLFWwindow*)_window->window, width, height);
}

void GLFWWindowImpl::SetTitle(WindowHandle* _window, const char* _new_title) {
    glfwSetWindowTitle((GLFWwindow*)_window->window, _new_title);
}

bool GLFWWindowImpl::ShouldClose(WindowHandle* _window) const {
    return glfwWindowShouldClose((GLFWwindow*)_window->window);
}
void* GLFWWindowImpl::GetInteropHandle(WindowHandle* _window, EWindowInteropHandleType type) const {
    if (type != EWindowInteropHandleType::PlatformWindow) {
        return nullptr;
    }
#if PLATFORM_WINDOWS
    return glfwGetWin32Window((GLFWwindow*)_window->window);
#endif
    return nullptr;
}

} // namespace Moer
