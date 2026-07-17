#include "GLFWWindowImpl.h"

#include "Core.h"
#include "misc/MMemory.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "window/WindowContext.h"

// 1： define vulkan ahead of glfw
#include "rhi/vulkan/VulkanPlatform.h"
// 2
#include "GLFW/glfw3.h"

#include "IconsFontAwesome6.h"
#include <fstream>
#if PLATFORM_WINDOWS
//for dx12
//https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include <imgui.h>

#include "log/LogSystem.h"
#include <string.h>

#include "window/WindowInput.h"

namespace Moer {

//------------------------call back functions---------------------------
static void UpdateKeyStateWithActionIsPress(bool& key_state, int action);                   // tool func
static bool UpdateKeyStateWhenBoolExpression(bool& key_state, bool expression, int action); // tool func
static void UpdateAllKeyStates(int key, int action, int mods);                              // tool func
static void KeyCallbackFunc(GLFWwindow* window, int key, int scancode, int action, int mods);
static void CursorPosCallbackFunc(GLFWwindow* window, double xpos, double ypos);
static void FrameBufferSizeCallbackFunc(GLFWwindow* window, int width, int height);
static void ScrollCallbackFunc(GLFWwindow* window, double xoffset, double yoffset);
static void MouseButtonCallbackFunc(GLFWwindow* window, int button, int action, int mode);

GLFWWindowImpl::GLFWWindowImpl() {}
GLFWWindowImpl::~GLFWWindowImpl() {}
void GLFWWindowImpl::PollEvents() const {
    glfwPollEvents();
}

void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
    if (!glfwInit()) {
        //error log and quit
        LOG_ERROR("Window init fail.");
        assert(0 && "Window init fail.");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (info.rhi_type == ERHIType::D3D12) {
        InitD3D12();
    } else if (info.rhi_type == ERHIType::Vulkan) {
        InitVulkan();
    } else {
        assert(0 && "Unknown RHI type, code error.");
    }
    glfwWindowHint(GLFW_VISIBLE, info.b_visible ? GLFW_TRUE : GLFW_FALSE);

    int          width   = info.width;
    int          height  = info.height;
    GLFWmonitor* monitor = nullptr;
    m_deferred_fullscreen        = false;
    m_deferred_fullscreen_width  = 0;
    m_deferred_fullscreen_height = 0;
    if (info.b_fullscreen) {
        // 全屏模式
        GLFWmonitor*       fullscreen_monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode               = glfwGetVideoMode(fullscreen_monitor);
        width                                 = mode->width;
        height                                = mode->height;

        if (info.b_visible) {
            monitor = fullscreen_monitor;
        } else {
            // GLFW_VISIBLE is ignored for full-screen windows. Bootstrap as a
            // hidden windowed window at the target mode size, then enter full
            // screen from ShowMainWindow() after the first frame is ready.
            m_deferred_fullscreen        = true;
            m_deferred_fullscreen_width  = width;
            m_deferred_fullscreen_height = height;
        }
    }

    GLFWwindow* window = glfwCreateWindow(width, height, info.title.c_str(), monitor, nullptr);
    // Window hints persist across creations. Restore the normal default so editor platform
    // windows are not accidentally created hidden after a hidden main-window bootstrap.
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    glfwSetWindowUserPointer(window, this);

    this->main_window_handle.window = window;

    GuiWindowInitInfo window_info{.window = window, .b_install_callbacks = true, .rhi_type = info.rhi_type};

    //register engine io callbacks MARK.. remains problems
    InstallInterface(&main_window_handle);

    //install imgui io callbacks
    // GuiInit(window_info);
    // ImGui::CreateContext();
    // ImGui_ImplGlfw_InitForVulkan(window, true);
}

void GLFWWindowImpl::InstallInterface(WindowHandle* _handle) {
    GLFWwindow* window = (GLFWwindow*)_handle->window;

    {
        if (GLFWcharfun fc = glfwSetCharCallback(window, [](GLFWwindow* window, unsigned int codepoint) {
                WindowImpl::OnCharCallback((WindowType*)window, codepoint);
            }))
            RegisterOnCharFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
    }
    {
        GLFWcursorenterfun fc = glfwSetCursorEnterCallback(window, [](GLFWwindow* window, int entered) {
            WindowImpl::OnCursorEnterCallback(window, entered);
        });
        if (fc)
            RegisterOnCursorEnterFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
    }
    {
        //modified
        auto fc = glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) {
            WindowImpl::OnCursorPosCallback(window, xpos, ypos);
        });
        // if(fc)
        // RegisterOnCursorPosFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        RegisterOnCursorPosFunc(
            window,
            std::bind(
                CursorPosCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2
            )
        );
    }
    {
        auto fc = glfwSetDropCallback(window, [](GLFWwindow* window, int path_count, const char** paths) {
            WindowImpl::OnDropCallback(window, path_count, paths);
        });
        if (fc)
            RegisterOnDropFunc(
                window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2)
            );
    }
    {
        auto fc = glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) {
            WindowImpl::OnFramebufferSizeCallback(window, width, height);
        });
        RegisterOnFrameBufferSizeFunc(
            window,
            std::bind(
                FrameBufferSizeCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2
            )
        );
        // if (fc)
        // RegisterOnFrameBufferSizeFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
    }
    {
        //modified
        auto fc =
            glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
                WindowImpl::OnKeyCallback(window, key, scancode, action, mods);
            });
        RegisterOnKeyFunc(
            window,
            std::bind(
                KeyCallbackFunc,
                (GLFWwindow*)window,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3,
                std::placeholders::_4
            )
        );
        // if(fc)
        // RegisterOnKeyFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
    }
    {
        auto fc =
            glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mode) {
                WindowImpl::OnMouseButtonCallback(window, button, action, mode);
            });
        RegisterOnMouseButtonFunc(
            window,
            std::bind(
                MouseButtonCallbackFunc,
                (GLFWwindow*)window,
                std::placeholders::_1,
                std::placeholders::_2,
                std::placeholders::_3
            )
        );
        // if (fc)
        // RegisterOnMouseButtonFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }
    {
        auto fc = glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) {
            WindowImpl::OnScrollCallback(window, xoffset, yoffset);
        });
        RegisterOnScrollFunc(
            window,
            std::bind(ScrollCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2)
        );
        // if (fc)
        // RegisterOnScrollFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
    }
    {
        auto fc = glfwSetWindowCloseCallback(window, [](GLFWwindow* window) {
            WindowImpl::OnWindowCloseCallback(window);
        });
        if (fc)
            RegisterOnWindowCloseFunc(window, std::bind(fc, (GLFWwindow*)window));
    }
    {
        auto fc =
            glfwSetWindowContentScaleCallback(window, [](GLFWwindow* window, float xscale, float yscale) {
                WindowImpl::OnWindowContentScaleCallback(window, xscale, yscale);
            });
        if (fc)
            RegisterOnWindowContentScaleFunc(
                window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2)
            );
    }
    {
        auto fc = glfwSetWindowPosCallback(window, [](GLFWwindow* window, int xpos, int ypos) {
            WindowImpl::OnWindowPosCallback(window, xpos, ypos);
        });
        if (fc)
            RegisterOnWindowPosFunc(
                window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2)
            );
    }
    {
        auto fc = glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) {
            WindowImpl::OnWindowSizeCallback(window, width, height);
        });
        if (fc)
            RegisterOnWindowSizeFunc(
                window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2)
            );
    }
    {
        auto fc = glfwSetWindowFocusCallback(window, [](GLFWwindow* window, int focused) {
            WindowImpl::OnWindowFocusCallback(window, focused);
        });
        if (fc)
            RegisterOnWindowFocusFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
    }
}

void GLFWWindowImpl::Tick() {
    // per-frame time logic
    float current_frame_time           = static_cast<float>(glfwGetTime());
    WindowInput::Get().delta_time      = current_frame_time - WindowInput::Get().last_frame_time;
    WindowInput::Get().last_frame_time = current_frame_time;

    PollEvents();
    // GuiUpdate();

    TickCursorState();
}
void GLFWWindowImpl::ShutDown() {
    // GuiShutDown();
    glfwDestroyWindow((GLFWwindow*)main_window_handle.window);
}

void GLFWWindowImpl::TickCursorState() {
    // hide cursor when **left or right** mouse button is pressed
    bool b_should_hide = WindowInput::Get().mouse_button_state[MouseButtons::Left] ||
                         WindowInput::Get().mouse_button_state[MouseButtons::Right] ||
                         WindowInput::Get().mouse_button_state[MouseButtons::Middle] ||
                         WindowInput::Get().key_button_switch_state[KeyButtons::F];

    // is_active <=> SceneColor has captured camera input
    if (WindowInput::Get().is_active && b_should_hide) {
        SetCursorHide();
    } else {
        SetCursorNormal();
    }
}

void GLFWWindowImpl::SetCursorHide() {
    auto& input = WindowInput::Get();
    if (!input.is_cursor_hiding) {
        input.is_cursor_dirty = true;
        input.cursor_delta_x  = 0.0f;
        input.cursor_delta_y  = 0.0f;
    }

    glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#ifdef GLFW_RAW_MOUSE_MOTION
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
#endif
    input.is_cursor_hiding = true;
}

void GLFWWindowImpl::SetCursorNormal() {
#ifdef GLFW_RAW_MOUSE_MOTION
    if (WindowInput::Get().is_cursor_hiding && glfwRawMouseMotionSupported()) {
        glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
#endif
    glfwSetInputMode((GLFWwindow*)main_window_handle.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    WindowInput::Get().is_cursor_hiding = false;
    WindowInput::Get().is_cursor_dirty  = true;
    WindowInput::Get().cursor_delta_x   = 0.0f;
    WindowInput::Get().cursor_delta_y   = 0.0f;
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
// void GLFWWindowImpl::CreateVulkanSurface(void* instance, WindowType* window, void* allocation_callback, void* surface) {
//     glfwCreateWindowSurface((VkInstance)instance, (GLFWwindow*)window, (const VkAllocationCallbacks*)allocation_callback, (VkSurfaceKHR*)surface);
// }
void GLFWWindowImpl::CreateVulkanSurface(
    void*         instance,
    WindowHandle* window,
    void*         allocation_callback,
    void*         surface
) {
    glfwCreateWindowSurface(
        (VkInstance)instance,
        (GLFWwindow*)window->window,
        (const VkAllocationCallbacks*)allocation_callback,
        (VkSurfaceKHR*)surface
    );
}
void GLFWWindowImpl::SetFocusMode(WindowHandle* _window, bool _focused) {
    glfwSetInputMode(
        (GLFWwindow*)_window->window, GLFW_CURSOR, focused ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
    );
}

void GLFWWindowImpl::GetWindowSize(WindowHandle* _window, int32_t* width, int32_t* height) const {
    glfwGetWindowSize((GLFWwindow*)_window->window, width, height);
}

void GLFWWindowImpl::SetTitle(WindowHandle* _window, const char* _new_title) {
    glfwSetWindowTitle((GLFWwindow*)_window->window, _new_title);
}

void GLFWWindowImpl::RequestClose(WindowHandle* _window) {
    glfwSetWindowShouldClose((GLFWwindow*)_window->window, GLFW_TRUE);
}

bool GLFWWindowImpl::ShouldClose(WindowHandle* _window) const {
    return glfwWindowShouldClose((GLFWwindow*)_window->window);
}
void GLFWWindowImpl::ShowMainWindow() {
    assert(IsCurrentlyGameThread() && "GLFW main-window visibility must be changed on the Game Thread.");
    auto* window = (GLFWwindow*)main_window_handle.window;
    if (m_deferred_fullscreen) {
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            // glfwGetError reports and clears the oldest pending error. Clear
            // unrelated startup errors so the result below belongs to this
            // monitor transition.
            glfwGetError(nullptr);
            glfwSetWindowMonitor(
                window,
                monitor,
                0,
                0,
                m_deferred_fullscreen_width,
                m_deferred_fullscreen_height,
                GLFW_DONT_CARE
            );
            const char* glfw_error_description = nullptr;
            const int   glfw_error = glfwGetError(&glfw_error_description);
            if (glfw_error == GLFW_NO_ERROR) {
                LOG_INFO(
                    "[Startup][Window] Entered deferred full-screen mode at {}x{}.",
                    m_deferred_fullscreen_width,
                    m_deferred_fullscreen_height
                );
            } else {
                LOG_ERROR(
                    "[Startup][Window] Failed to enter deferred full-screen mode: error={}, description={}",
                    glfw_error,
                    glfw_error_description ? glfw_error_description : "<none>"
                );
            }
        } else {
            LOG_WARNING(
                "Unable to enter deferred full-screen mode because no primary monitor is available."
            );
        }
        m_deferred_fullscreen = false;
    }
    glfwShowWindow(window);
}
void* GLFWWindowImpl::GetNativeWindow(WindowHandle* _window) const {
#if PLATFORM_WINDOWS
    return glfwGetWin32Window((GLFWwindow*)_window->window);
#endif
    return nullptr;
}

void GLFWWindowImpl::OnCursorEnterCallbackImpl(WindowType* window, int entered) {
    OnCursorEnter(window, entered);
};

void GLFWWindowImpl::OnCharCallbackImpl(WindowType* window, unsigned int codepoint) {
    OnChar(window, codepoint);
}

void GLFWWindowImpl::OnCursorPosCallbackImpl(WindowType* window, double xpos, double ypos) {
    OnCursorPos(window, xpos, ypos);
}
void GLFWWindowImpl::OnDropCallbackImpl(WindowType* window, int path_count, const char** paths) {
    OnDrop(window, path_count, paths);
}
void GLFWWindowImpl::OnFramebufferSizeCallbackImpl(WindowType* window, int width, int height) {
    OnFramebufferSize(window, width, height);
}
void GLFWWindowImpl::OnKeyCallbackImpl(WindowType* window, int key, int scancode, int action, int mods) {
    OnKey(window, key, scancode, action, mods);
}
void GLFWWindowImpl::OnMouseButtonCallbackImpl(WindowType* window, int button, int action, int mode) {
    OnMouseButton(window, button, action, mode);
}
void GLFWWindowImpl::OnScrollCallbackImpl(WindowType* window, double xoffset, double yoffset) {
    OnScroll(window, xoffset, yoffset);
}
void GLFWWindowImpl::OnWindowCloseCallbackImpl(WindowType* window) {
    OnWindowClose(window);
}
void GLFWWindowImpl::OnWindowContentScaleCallbackImpl(WindowType* window, float xscale, float yscale) {
    OnWindowContentScale(window, xscale, yscale);
}
void GLFWWindowImpl::OnWindowPosCallbackImpl(WindowType* window, int xpos, int ypos) {
    OnWindowPos(window, xpos, ypos);
}
void GLFWWindowImpl::OnWindowSizeCallbackImpl(WindowType* window, int width, int height) {
    OnWindowSize(window, width, height);
}
void GLFWWindowImpl::OnWindowFocusCallbackImpl(WindowType* window, int focused) {
    OnWindowFocus(window, focused);
}

static void UpdateKeyStateWithActionIsPress(bool& key_state, int action) {
    if (action == GLFW_PRESS) {
        key_state = true;
    } else if (action == GLFW_RELEASE) {
        key_state = false;
    }
}

static bool UpdateKeyStateWhenBoolExpression(bool& key_state, bool expression, int action) {
    if (expression) {
        UpdateKeyStateWithActionIsPress(key_state, action);
        return true;
    }
    return false;
}

static void UpdateCameraControlState(int key, int action, int mods) {
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_forward, key == GLFW_KEY_W, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_backward, key == GLFW_KEY_S, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_left, key == GLFW_KEY_A, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_right, key == GLFW_KEY_D, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_up, key == GLFW_KEY_E, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().camera_down, key == GLFW_KEY_Q, action))
        return;

    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().speed_up, key == GLFW_KEY_UP, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(WindowInput::Get().speed_down, key == GLFW_KEY_DOWN, action))
        return;
    if (UpdateKeyStateWhenBoolExpression(
            WindowInput::Get().reset_speed, key == GLFW_KEY_KP_0 && mods == GLFW_MOD_CONTROL, action
        ))
        return;
}

static void UpdateAllKeyStates(int key, int action, int mods) {
    // Generated by copilot
    static const std::unordered_map<int, int> key_map = {
        {GLFW_KEY_A, KeyButtons::A},           {GLFW_KEY_B, KeyButtons::B},
        {GLFW_KEY_C, KeyButtons::C},           {GLFW_KEY_D, KeyButtons::D},
        {GLFW_KEY_E, KeyButtons::E},           {GLFW_KEY_F, KeyButtons::F},
        {GLFW_KEY_G, KeyButtons::G},           {GLFW_KEY_H, KeyButtons::H},
        {GLFW_KEY_I, KeyButtons::I},           {GLFW_KEY_J, KeyButtons::J},
        {GLFW_KEY_K, KeyButtons::K},           {GLFW_KEY_L, KeyButtons::L},
        {GLFW_KEY_M, KeyButtons::M},           {GLFW_KEY_N, KeyButtons::N},
        {GLFW_KEY_O, KeyButtons::O},           {GLFW_KEY_P, KeyButtons::P},
        {GLFW_KEY_Q, KeyButtons::Q},           {GLFW_KEY_R, KeyButtons::R},
        {GLFW_KEY_S, KeyButtons::S},           {GLFW_KEY_T, KeyButtons::T},
        {GLFW_KEY_U, KeyButtons::U},           {GLFW_KEY_V, KeyButtons::V},
        {GLFW_KEY_W, KeyButtons::W},           {GLFW_KEY_X, KeyButtons::X},
        {GLFW_KEY_Y, KeyButtons::Y},           {GLFW_KEY_Z, KeyButtons::Z},
        {GLFW_KEY_UP, KeyButtons::UP},         {GLFW_KEY_DOWN, KeyButtons::DOWN},
        {GLFW_KEY_LEFT, KeyButtons::LEFT},     {GLFW_KEY_RIGHT, KeyButtons::RIGHT},
        {GLFW_KEY_ESCAPE, KeyButtons::ESCAPE},
    };
    if (key_map.find(key) == key_map.end()) {
        return;
    }
    auto cameraKey = key_map.at(key);
    UpdateKeyStateWithActionIsPress(WindowInput::Get().key_button_state[cameraKey], action);
    if (action == GLFW_RELEASE) { // when key is pressed, switch the state
        WindowInput::Get().key_button_switch_state[cameraKey] ^= 1;
    }
}

static void KeyCallbackFunc(GLFWwindow* window, int key, int scancode, int action, int mods) {
    UpdateCameraControlState(key, action, mods);
    UpdateAllKeyStates(key, action, mods);
}

static void CursorPosCallbackFunc(GLFWwindow* window, double xpos, double ypos) {
    float xPos = static_cast<float>(xpos);
    float yPos = static_cast<float>(ypos);

    auto& input = WindowInput::Get();

    if (input.is_cursor_dirty) {
        input.cursor_last_x   = xPos;
        input.cursor_last_y   = yPos;
        input.is_cursor_dirty = false;
    }

    input.cursor_delta_x += xPos - input.cursor_last_x;
    input.cursor_delta_y += yPos - input.cursor_last_y;

    input.cursor_last_x = xPos;
    input.cursor_last_y = yPos;
}

static void FrameBufferSizeCallbackFunc(GLFWwindow* window, int width, int height) {
    WindowInput::Get().width        = width;
    WindowInput::Get().height       = height;
    WindowInput::Get().aspect_ratio = height == 0 ? 0 : (float)width / height;
}

static void ScrollCallbackFunc(GLFWwindow* window, double xoffset, double yoffset) {
    WindowInput::Get().scroll_offset = static_cast<float>(yoffset);
}

static void MouseButtonCallbackFunc(GLFWwindow* window, int button, int action, int mode) {
    static const std::unordered_map<int, int> mouse_button_map = {
        {GLFW_MOUSE_BUTTON_LEFT, MouseButtons::Left},
        {GLFW_MOUSE_BUTTON_MIDDLE, MouseButtons::Middle},
        {GLFW_MOUSE_BUTTON_RIGHT, MouseButtons::Right},
    };
    if (mouse_button_map.find(button) == mouse_button_map.end()) {
        return;
    }
    auto cameraButton = mouse_button_map.at(button);
    UpdateKeyStateWithActionIsPress(WindowInput::Get().mouse_button_state[cameraButton], action);
}

} // namespace Moer
