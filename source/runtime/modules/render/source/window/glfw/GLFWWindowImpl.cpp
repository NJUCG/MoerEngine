#include "GLFWWindowImpl.h"
#include "config/ConfigManager.h"
#include "rhi/RHI.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "window/WindowContext.h"
#include "platform/Platform.h"
//define vulkan ahead of glfw
#include "vulkan/vulkan.h"
#include "GLFW/glfw3.h"
#include "IconsFontAwesome6.h"
#include <fstream>
#if PLATFORM_WINDOWS
//for dx12
//https://docs.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif
#include "GLFWUIImpl.h"

#include <imgui.h>

#include "log/LogSystem.h"
#include <string.h>

namespace Moer {

    GLFWWindowImpl::GLFWWindowImpl() {
    }
    GLFWWindowImpl::~GLFWWindowImpl() {
    }
    void GLFWWindowImpl::PollEvents() const { glfwPollEvents(); }

    const ImWchar* FontTypeToRange(Moer::EFontType _font_range_type) {
        using namespace Moer;
        static const ImWchar icons_ranges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};

        switch (_font_range_type) {
            case EFontType::Greek:
                return ImGui::GetIO().Fonts->GetGlyphRangesGreek();
            case EFontType::Chinese:
                return ImGui::GetIO().Fonts->GetGlyphRangesChineseFull();
            case EFontType::Korean:
                return ImGui::GetIO().Fonts->GetGlyphRangesKorean();
            case EFontType::Japanese:
                return ImGui::GetIO().Fonts->GetGlyphRangesJapanese();
            case EFontType::Cyrillic:
                return ImGui::GetIO().Fonts->GetGlyphRangesCyrillic();
            case EFontType::Thai:
                return ImGui::GetIO().Fonts->GetGlyphRangesThai();
            case EFontType::Vietnamese:
                return ImGui::GetIO().Fonts->GetGlyphRangesVietnamese();
            case EFontType::Default:
                return ImGui::GetIO().Fonts->GetGlyphRangesDefault();
            case EFontType::Icon:
                return icons_ranges;
            default:
                break;
        }
        return ImGui::GetIO().Fonts->GetGlyphRangesDefault();
    }

    void GLFWWindowImpl::GuiInit(const GuiWindowInitInfo& _init_info) {

        GuiWindowInit(_init_info);
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        //icon fonts
        {
            AddFont({FONT_ICON_FILE_NAME_FAS,
                     13.0f,
                     EFontType::Icon});
        }

        {
            AddFont({"msyh.ttc",
                     20.0f,
                     EFontType::Chinese});
        }
    };

    void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
        if (!glfwInit()) {
            //error log and quit
            LOG_ERROR("Window init fail.");
            assert(0 && "Window init fail.");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        if (info.rhi_name == "D3D12") {
            InitD3D12();
        } else {
            InitVulkan();
        }

        GLFWwindow* window = glfwCreateWindow(info.width, info.height, info.title.c_str(), nullptr, nullptr);

        glfwSetWindowUserPointer(window, this);

        this->main_window_handle.window = window;

        GuiWindowInitInfo window_info{.window              = window,
                                      .b_install_callbacks = true,
                                      .rhi_type            = g_rhi->GetType()};

        //register engine io callbacks MARK.. remains problems
        InstallInterface(&main_window_handle);
        //install imgui io callbacks
        GuiInit(window_info);
        // ImGui::CreateContext();
        // ImGui_ImplGlfw_InitForVulkan(window, true);
    }

    void GLFWWindowImpl::InstallInterface(WindowHandle* _handle) {
        GLFWwindow* window = (GLFWwindow*)_handle->window;
        {
            if (GLFWcharfun fc = glfwSetCharCallback(window, [](GLFWwindow* window, unsigned int codepoint) { WindowImpl::OnCharCallback((WindowType*)window, codepoint); }))
                RegisterOnCharFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
        {
            GLFWcursorenterfun fc = glfwSetCursorEnterCallback(window, [](GLFWwindow* window, int entered) { WindowImpl::OnCursorEnterCallback(window, entered); });
            if (fc)
                RegisterOnCursorEnterFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
        {
            auto fc = glfwSetCursorPosCallback(window, [](GLFWwindow* window, double xpos, double ypos) { WindowImpl::OnCursorPosCallback(window, xpos, ypos); });
            if (fc)
                RegisterOnCursorPosFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetDropCallback(window, [](GLFWwindow* window, int path_count, const char** paths) { WindowImpl::OnDropCallback(window, path_count, paths); });
            if (fc)
                RegisterOnDropFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetFramebufferSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowImpl::OnFramebufferSizeCallback(window, width, height); });
            if (fc)
                RegisterOnFrameBufferSizeFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) { WindowImpl::OnKeyCallback(window, key, scancode, action, mods); });
            if (fc)
                RegisterOnKeyFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
        }
        {
            auto fc = glfwSetMouseButtonCallback(window, [](GLFWwindow* window, int button, int action, int mode) { WindowImpl::OnMouseButtonCallback(window, button, action, mode); });
            if (fc)
                RegisterOnMouseButtonFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        }
        {
            auto fc = glfwSetScrollCallback(window, [](GLFWwindow* window, double xoffset, double yoffset) { WindowImpl::OnScrollCallback(window, xoffset, yoffset); });
            if (fc)
                RegisterOnScrollFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowCloseCallback(window, [](GLFWwindow* window) { WindowImpl::OnWindowCloseCallback(window); });
            if (fc)
                RegisterOnWindowCloseFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window));
        }
        {
            auto fc = glfwSetWindowContentScaleCallback(window, [](GLFWwindow* window, float xscale, float yscale) { WindowImpl::OnWindowContentScaleCallback(window, xscale, yscale); });
            if (fc)
                RegisterOnWindowContentScaleFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowPosCallback(window, [](GLFWwindow* window, int xpos, int ypos) { WindowImpl::OnWindowPosCallback(window, xpos, ypos); });
            if (fc)
                RegisterOnWindowPosFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowSizeCallback(window, [](GLFWwindow* window, int width, int height) { WindowImpl::OnWindowSizeCallback(window, width, height); });
            if (fc)
                RegisterOnWindowSizeFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1, std::placeholders::_2));
        }
        {
            auto fc = glfwSetWindowFocusCallback(window, [](GLFWwindow* window, int focused) { WindowImpl::OnWindowFocusCallback(window, focused); });
            if (fc)
                RegisterOnWindowFocusFunc(&main_window_handle, std::bind(fc, (GLFWwindow*)window, std::placeholders::_1));
        }
    }

    void GLFWWindowImpl::AddFont(const FontDesc& _desc) {
        const auto& font_base_path = Moer::ConfigManager::GetInstance().GetEditorResourcePath() / FONTS_DIR;
        const auto& font_path      = font_base_path / _desc.font_path;

        auto& io = ImGui::GetIO();

        const ImWchar* font_range = FontTypeToRange(_desc.font_type);
        ImFontConfig   icons_config;
        icons_config.MergeMode            = false;
        icons_config.PixelSnapH           = true;
        icons_config.FontDataOwnedByAtlas = false;
        if (_desc.font_type == Moer::EFontType::Icon) {
            float icon_font_size = _desc.font_size * 2.0f / 3.0f;// FontAwesome fonts need to have their sizes reduced by 2.0f/3.0f in order to align correctly

            icons_config.MergeMode        = true;
            icons_config.GlyphMinAdvanceX = icon_font_size;

            io.Fonts->AddFontFromFileTTF(font_path.generic_string().data(), icon_font_size, &icons_config, font_range);
        } else {
            io.FontDefault = io.Fonts->AddFontFromFileTTF(font_path.generic_string().data(), _desc.font_size, &icons_config, font_range);
        }
    }

    void GLFWWindowImpl::Tick() {
        PollEvents();
        GuiUpdate();
    }
    void GLFWWindowImpl::ShutDown() {
        GuiShutDown();
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
    void GLFWWindowImpl::GuiUpdate() {
        GuiWindowNewFrame();
    }
    void GLFWWindowImpl::GuiShutDown() {
        GuiWindowShutDown();
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

}// namespace Moer
