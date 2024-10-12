#include "GLFWWindowImpl.h"
#include "config/ConfigManager.h"
#include "misc/MMemory.h"
#include "rhi/RHI.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "window/WindowContext.h"
#include "platform/Platform.h"
//define vulkan ahead of glfw
#include <volk.h>
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
    WindowInput& wndInput = WindowInput::GetInstance();

    //------------------------call back functions---------------------------
    void KeyCallbackFunc(GLFWwindow* window, int key, int scancode, int action, int mods);
    void CursorPosCallbackFunc(GLFWwindow* window, double xpos, double ypos);
    void FrameBufferSizeCallbackFunc(GLFWwindow* window, int width, int height);
    void ScrollCallbackFunc(GLFWwindow* window, double xoffset, double yoffset);
    void MouseButtonCallbackFunc(GLFWwindow* window, int button, int action, int mode);

    GLFWWindowImpl::GLFWWindowImpl() {
    }
    GLFWWindowImpl::~GLFWWindowImpl() {
    }
    void GLFWWindowImpl::PollEvents() const { glfwPollEvents(); }

    void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
        if (!glfwInit()) {
            //error log and quit
            LOG_ERROR("Window init fail.");
            assert(0 && "Window init fail.");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        ERHIType rhi_type = ERHIType::Vulkan;
        if (info.rhi_name == "D3D12") {
            InitD3D12();
            rhi_type = ERHIType::D3D12;
        } else {
            InitVulkan();
            rhi_type = ERHIType::Vulkan;
        }

        GLFWwindow* window = glfwCreateWindow(info.width, info.height, info.title.c_str(), nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);

        this->main_window_handle.window = window;

        GuiWindowInitInfo window_info{.window              = window,
                                      .b_install_callbacks = true,
                                      .rhi_type            = rhi_type};

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
            if (GLFWcharfun fc = glfwSetCharCallback(window, [](GLFWwindow* window, unsigned int codepoint) { WindowImpl::OnCharCallback((WindowType*)window, codepoint); }))
                RegisterOnCharFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
        {
            GLFWcursorenterfun fc = glfwSetCursorEnterCallback(window, [](GLFWwindow* window, int entered) { WindowImpl::OnCursorEnterCallback(window, entered); });
            if (fc)
                RegisterOnCursorEnterFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
        {
            //modified
            auto fc = glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) { WindowImpl::OnCursorPosCallback(window, xpos, ypos); });
            // if(fc)
            // RegisterOnCursorPosFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
            RegisterOnCursorPosFunc(window, std::bind(CursorPosCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetDropCallback(window, [](GLFWwindow* window, int path_count, const char** paths) { WindowImpl::OnDropCallback(window, path_count, paths); });
            if (fc)
                RegisterOnDropFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowImpl::OnFramebufferSizeCallback(window, width, height); });
            RegisterOnFrameBufferSizeFunc(window, std::bind(FrameBufferSizeCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
            // if (fc)
            // RegisterOnFrameBufferSizeFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            //modified
            auto fc = glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) { WindowImpl::OnKeyCallback(window, key, scancode, action, mods); });
            RegisterOnKeyFunc(window, std::bind(KeyCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
            // if(fc)
            // RegisterOnKeyFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        }
        {
            auto fc = glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mode) { WindowImpl::OnMouseButtonCallback(window, button, action, mode); });
            RegisterOnMouseButtonFunc(window, std::bind(MouseButtonCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
            // if (fc)
            // RegisterOnMouseButtonFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        }
        {
            auto fc = glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) { WindowImpl::OnScrollCallback(window, xoffset, yoffset); });
            RegisterOnScrollFunc(window, std::bind(ScrollCallbackFunc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
            // if (fc)
            // RegisterOnScrollFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowCloseCallback(window, [](GLFWwindow* window) { WindowImpl::OnWindowCloseCallback(window); });
            if (fc)
                RegisterOnWindowCloseFunc(window, std::bind(fc, (GLFWwindow*)window));
        }
        {
            auto fc = glfwSetWindowContentScaleCallback(window, [](GLFWwindow* window, float xscale, float yscale) { WindowImpl::OnWindowContentScaleCallback(window, xscale, yscale); });
            if (fc)
                RegisterOnWindowContentScaleFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowPosCallback(window, [](GLFWwindow* window, int xpos, int ypos) { WindowImpl::OnWindowPosCallback(window, xpos, ypos); });
            if (fc)
                RegisterOnWindowPosFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowImpl::OnWindowSizeCallback(window, width, height); });
            if (fc)
                RegisterOnWindowSizeFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowFocusCallback(window, [](GLFWwindow* window, int focused) { WindowImpl::OnWindowFocusCallback(window, focused); });
            if (fc)
                RegisterOnWindowFocusFunc(window, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
    }

    void GLFWWindowImpl::Tick() {
        // per-frame time logic
        float currentFrame = static_cast<float>(glfwGetTime());
        wndInput.deltaTime = currentFrame - wndInput.lastFrame;
        wndInput.lastFrame = currentFrame;

        PollEvents();
        // GuiUpdate();
    }
    void GLFWWindowImpl::ShutDown() {
        // GuiShutDown();
        glfwDestroyWindow((GLFWwindow*)main_window_handle.window);
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
    void GLFWWindowImpl::CreateVulkanSurface(void* instance, WindowHandle* window, void* allocation_callback, void* surface) {
        glfwCreateWindowSurface((VkInstance)instance, (GLFWwindow*)window->window, (const VkAllocationCallbacks*)allocation_callback, (VkSurfaceKHR*)surface);
    }
    void GLFWWindowImpl::SetFocusMode(WindowHandle* _window, bool _focused) {
        glfwSetInputMode((GLFWwindow*)_window->window, GLFW_CURSOR, focused ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
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

    void KeyCallbackFunc(GLFWwindow* window, int key, int scancode, int action, int mods) {
        // if(key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        // glfwSetWindowShouldClose(window, GL_TRUE);

        if (key == GLFW_KEY_F && action == GLFW_PRESS) {
            if (!wndInput.mouseEnterScreen) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                wndInput.mouseEnterScreen = true;
            } else {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                wndInput.mouseEnterScreen = false;
                wndInput.firstMouse       = true;
            }
        }
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            wndInput.mouseEnterScreen = false;
            wndInput.firstMouse       = true;
        }

        if (wndInput.mouseEnterScreen) {
            if (key == GLFW_KEY_W) {
                if (action == GLFW_PRESS)
                    wndInput.camera_forward = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_forward = false;
            }
            if (key == GLFW_KEY_S) {
                if (action == GLFW_PRESS)
                    wndInput.camera_backward = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_backward = false;
            }
            if (key == GLFW_KEY_A) {
                if (action == GLFW_PRESS)
                    wndInput.camera_left = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_left = false;
            }
            if (key == GLFW_KEY_D) {
                if (action == GLFW_PRESS)
                    wndInput.camera_right = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_right = false;
            }
            if (key == GLFW_KEY_SPACE) {
                if (action == GLFW_PRESS)
                    wndInput.camera_up = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_up = false;
            }
            if (key == GLFW_KEY_C) {
                if (action == GLFW_PRESS)
                    wndInput.camera_down = true;
                if (action == GLFW_RELEASE)
                    wndInput.camera_down = false;
            }
            if (key == GLFW_KEY_UP) {
                if (action == GLFW_PRESS)
                    wndInput.speedUp = true;
                if (action == GLFW_RELEASE)
                    wndInput.speedUp = false;
            }
            if (key == GLFW_KEY_DOWN) {
                if (action == GLFW_PRESS)
                    wndInput.speedDown = true;
                if (action == GLFW_RELEASE)
                    wndInput.speedDown = false;
            }
            if (key == GLFW_KEY_KP_0 && mods == GLFW_MOD_CONTROL) {
                if (action == GLFW_PRESS)
                    wndInput.resetSpeed = true;
                if (action == GLFW_RELEASE)
                    wndInput.resetSpeed = false;
            }
        }
    }

    void CursorPosCallbackFunc(GLFWwindow* window, double xpos, double ypos) {
        if (wndInput.mouseEnterScreen) {
            float xPos = static_cast<float>(xpos);
            float yPos = static_cast<float>(ypos);

            if (wndInput.firstMouse) {
                wndInput.lastX      = xPos;
                wndInput.lastY      = yPos;
                wndInput.firstMouse = false;
            }

            wndInput.deltaX = xPos - wndInput.lastX;
            wndInput.deltaY = yPos - wndInput.lastY;

            wndInput.lastX = xPos;
            wndInput.lastY = yPos;
        }
    }

    void FrameBufferSizeCallbackFunc(GLFWwindow* window, int width, int height) {
        wndInput.width        = width;
        wndInput.height       = height;
        wndInput.aspect_ratio = height == 0 ? 0 : width / height;
    }

    void ScrollCallbackFunc(GLFWwindow* window, double xoffset, double yoffset) {
        if (wndInput.mouseEnterScreen) {
            wndInput.fov -= (float)yoffset * 2.f;
            if (wndInput.fov < 10.0f)
                wndInput.fov = 10.0f;
            if (wndInput.fov > 120.0f)
                wndInput.fov = 120.0f;
        }
    }

    void MouseButtonCallbackFunc(GLFWwindow* window, int button, int action, int mode) {
        static const std::unordered_map<int, int> mouseButtonMap = {
            {GLFW_MOUSE_BUTTON_LEFT, MouseButtons::Left},
            {GLFW_MOUSE_BUTTON_MIDDLE, MouseButtons::Middle},
            {GLFW_MOUSE_BUTTON_RIGHT, MouseButtons::Right},
        };
        if (wndInput.mouseEnterScreen) {
            if (mouseButtonMap.find(button) == mouseButtonMap.end()) {
                return;
            }
            auto cameraButton = mouseButtonMap.at(button);
            if (action == GLFW_PRESS) {
                wndInput.mouseButtonState[cameraButton] = true;
            } else {
                wndInput.mouseButtonState[cameraButton] = false;
            }
        }
    }

}// namespace Moer
