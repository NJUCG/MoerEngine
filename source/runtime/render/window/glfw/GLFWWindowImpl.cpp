#include "GLFWWindowImpl.h"

#include "Core.h"
#include "misc/MMemory.h"
#include "platform/Platform.h"
#include "rhi/vulkan/VulkanPlatform.h"
#include "window/WindowContext.h"

#include "GLFW/glfw3.h"
#include <vulkan/vulkan_core.h>

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

namespace {
class GLFWWindowSurfaceSource final : public Render::WindowSurfaceSource {
public:
    GLFWWindowSurfaceSource(void* window_system_handle, uintptr_t platform_window_handle) :
        window_system_handle(window_system_handle),
        platform_window_handle(platform_window_handle) {}

    Render::EWindowSystemType GetWindowSystem() const override {
        return Render::EWindowSystemType::GLFW;
    }

    void* GetWindowSystemHandle() const override {
        return window_system_handle;
    }

    uintptr_t GetPlatformWindowHandle() const override {
        return platform_window_handle;
    }

    void CreateSurface(
        ERHIType rhi_type,
        void*            instance,
        void*            allocation_callback,
        void*            surface
    ) const override {
        if (rhi_type == ERHIType::Vulkan) {
            const VkResult result = glfwCreateWindowSurface(
                static_cast<VkInstance>(instance),
                static_cast<GLFWwindow*>(window_system_handle),
                static_cast<const VkAllocationCallbacks*>(allocation_callback),
                static_cast<VkSurfaceKHR*>(surface)
            );
            MOER_ASSERT(
                result == VK_SUCCESS,
                "glfwCreateWindowSurface failed with VkResult={} while creating a GLFW-backed Vulkan surface",
                static_cast<int32_t>(result)
            );
            return;
        }

        MOER_ASSERT(
            false,
            "Unsupported RHI type for GLFWWindowSurfaceSource::CreateSurface: {}",
            static_cast<uint32_t>(rhi_type)
        );
    }

private:
    void*     window_system_handle   = nullptr;
    uintptr_t platform_window_handle = 0;
};
} // namespace

GLFWWindowImpl::GLFWWindowImpl() {}
GLFWWindowImpl::~GLFWWindowImpl() {}
void GLFWWindowImpl::PollEvents() const {
    glfwPollEvents();
}

void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
    if (!glfwInit()) {
        //error log and quit
        LOG_ERROR(MOER_TEXT("Window init fail."));
        MOER_ASSERT(false, "GLFW initialization failed");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

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

Render::SwapchainSurfaceInfo GLFWWindowImpl::CreateSwapchainSurfaceInfo(const WindowHandle& window) const {
    MOER_ASSERT(window.window != nullptr, "Swapchain surface creation requires a valid window handle");

    void* platform_window = GetInteropHandle(&window, EWindowInteropHandleType::PlatformWindow);
    MOER_ASSERT(
        platform_window != nullptr,
        "Swapchain surface creation requires a valid platform window handle"
    );

    return Render::SwapchainSurfaceInfo{
        .source = MakeShared<GLFWWindowSurfaceSource>(
            window.window, reinterpret_cast<uintptr_t>(platform_window)
        ),
    };
}

void* GLFWWindowImpl::GetInteropHandle(const WindowHandle* _window, EWindowInteropHandleType type) const {
    if (type == EWindowInteropHandleType::PlatformWindow) {
#if PLATFORM_WINDOWS
        return glfwGetWin32Window((GLFWwindow*)_window->window);
#else
        MOER_ASSERT(false, "Platform window interop is not supported on non-Windows platforms");
        return nullptr;
#endif
    }

    MOER_ASSERT(false, "Unsupported window interop handle type: {}", static_cast<uint32_t>(type));
    return nullptr;
}

} // namespace Moer
