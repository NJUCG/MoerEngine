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

int ToGlfwKey(KeyButtons key) {
    switch (key) {
        case KeyButtons::A: return GLFW_KEY_A;
        case KeyButtons::B: return GLFW_KEY_B;
        case KeyButtons::C: return GLFW_KEY_C;
        case KeyButtons::D: return GLFW_KEY_D;
        case KeyButtons::E: return GLFW_KEY_E;
        case KeyButtons::F: return GLFW_KEY_F;
        case KeyButtons::G: return GLFW_KEY_G;
        case KeyButtons::H: return GLFW_KEY_H;
        case KeyButtons::I: return GLFW_KEY_I;
        case KeyButtons::J: return GLFW_KEY_J;
        case KeyButtons::K: return GLFW_KEY_K;
        case KeyButtons::L: return GLFW_KEY_L;
        case KeyButtons::M: return GLFW_KEY_M;
        case KeyButtons::N: return GLFW_KEY_N;
        case KeyButtons::O: return GLFW_KEY_O;
        case KeyButtons::P: return GLFW_KEY_P;
        case KeyButtons::Q: return GLFW_KEY_Q;
        case KeyButtons::R: return GLFW_KEY_R;
        case KeyButtons::S: return GLFW_KEY_S;
        case KeyButtons::T: return GLFW_KEY_T;
        case KeyButtons::U: return GLFW_KEY_U;
        case KeyButtons::V: return GLFW_KEY_V;
        case KeyButtons::W: return GLFW_KEY_W;
        case KeyButtons::X: return GLFW_KEY_X;
        case KeyButtons::Y: return GLFW_KEY_Y;
        case KeyButtons::Z: return GLFW_KEY_Z;
        case KeyButtons::UP: return GLFW_KEY_UP;
        case KeyButtons::DOWN: return GLFW_KEY_DOWN;
        case KeyButtons::LEFT: return GLFW_KEY_LEFT;
        case KeyButtons::RIGHT: return GLFW_KEY_RIGHT;
        case KeyButtons::ESCAPE: return GLFW_KEY_ESCAPE;
        case KeyButtons::GRAVE_ACCENT: return GLFW_KEY_GRAVE_ACCENT;
        case KeyButtons::F5: return GLFW_KEY_F5;
        case KeyButtons::F8: return GLFW_KEY_F8;
        default: return GLFW_KEY_UNKNOWN;
    }
}

int ToGlfwMouseButton(MouseButtons button) {
    switch (button) {
        case MouseButtons::Left: return GLFW_MOUSE_BUTTON_LEFT;
        case MouseButtons::Middle: return GLFW_MOUSE_BUTTON_MIDDLE;
        case MouseButtons::Right: return GLFW_MOUSE_BUTTON_RIGHT;
        default: return GLFW_MOUSE_BUTTON_LEFT;
    }
}

void UpdateNativeInput(GLFWwindow* window) {
    WindowInput& input = WindowInput::Get();

    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
    const float2 cursor_pos{static_cast<float>(cursor_x), static_cast<float>(cursor_y)};
    if (input.native_mouse_dirty) {
        input.native_mouse_delta = {0.0f, 0.0f};
        input.native_mouse_dirty = false;
    } else {
        input.native_mouse_delta = {
            cursor_pos.x - input.native_mouse_pos.x,
            cursor_pos.y - input.native_mouse_pos.y,
        };
    }
    input.native_mouse_pos = cursor_pos;

    for (uint32_t i = 0; i < MouseButtons::MouseButtonCount; ++i) {
        input.native_mouse_button_down[i] = glfwGetMouseButton(
                                                window,
                                                ToGlfwMouseButton(static_cast<MouseButtons>(i))
                                            ) == GLFW_PRESS;
    }

    for (uint32_t i = 0; i < KeyButtons::KeyButtonCount; ++i) {
        const int glfw_key = ToGlfwKey(static_cast<KeyButtons>(i));
        const bool key_down = glfw_key != GLFW_KEY_UNKNOWN && glfwGetKey(window, glfw_key) == GLFW_PRESS;
        input.native_key_pressed[i]   = key_down && !input.native_key_last_down[i];
        input.native_key_released[i]  = !key_down && input.native_key_last_down[i];
        input.native_key_down[i]      = key_down;
        input.native_key_last_down[i] = key_down;
    }
}
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

    UpdateNativeInput((GLFWwindow*)main_window_handle.window);

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
    WindowInput& input = WindowInput::Get();
    if (!input.is_cursor_hiding) {
        glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        input.is_cursor_dirty   = true;
        input.native_mouse_dirty = true;
    }
    input.is_cursor_hiding = true;
}

void GLFWWindowImpl::SetCursorNormal() {
    WindowInput& input = WindowInput::Get();
    if (input.is_cursor_hiding) {
        glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        input.is_cursor_dirty   = true;
        input.native_mouse_dirty = true;
    }
    input.is_cursor_hiding = false;
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
