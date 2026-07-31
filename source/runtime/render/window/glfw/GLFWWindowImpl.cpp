#include "GLFWWindowImpl.h"

#include "Core.h"
#include "misc/MMemory.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/vulkan/VulkanCommon.h"
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
#include <exception>
#include <stdexcept>
#include <string.h>

namespace Moer {

struct GLFWWindowSurfaceLeaseState {
    std::atomic_uint32_t live_surface_leases{0};
};

namespace {

class GLFWInitTransaction {
public:
    explicit GLFWInitTransaction(WindowType*& published_window) : published_window(published_window) {}

    ~GLFWInitTransaction() {
        if (committed) {
            return;
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        published_window = nullptr;
        glfwTerminate();
    }

    GLFWInitTransaction(const GLFWInitTransaction&)            = delete;
    GLFWInitTransaction& operator=(const GLFWInitTransaction&) = delete;

    void Adopt(GLFWwindow* new_window) noexcept {
        window = new_window;
    }

    void Commit() noexcept {
        committed = true;
    }

private:
    WindowType*& published_window;
    GLFWwindow*  window{nullptr};
    bool         committed{false};
};

class GLFWWindowSurfaceSource final : public Render::WindowSurfaceSource {
public:
    GLFWWindowSurfaceSource(
        GLFWwindow*                            window,
        uintptr_t                              platform_window_handle,
        uint64_t                               generation,
        SharedPtr<GLFWWindowSurfaceLeaseState> lease_state
    ) :
        identity{
            .window_system          = Render::EWindowSystemType::GLFW,
            .window_system_handle   = reinterpret_cast<uintptr_t>(window),
            .platform_window_handle = platform_window_handle,
            .generation             = generation,
        },
        window(window),
        lease_state(std::move(lease_state)) {
        this->lease_state->live_surface_leases.fetch_add(1, std::memory_order_relaxed);
    }

    ~GLFWWindowSurfaceSource() override {
        lease_state->live_surface_leases.fetch_sub(1, std::memory_order_release);
    }

    [[nodiscard]] Render::WindowSurfaceIdentity GetIdentity() const noexcept override {
        return identity;
    }

    [[nodiscard]] Render::WindowSurfaceCreateResult
    CreateSurface(ERHIType rhi_type, void* instance, const void* allocation_callbacks, void* surface)
        const noexcept override {
        if (rhi_type != ERHIType::Vulkan) {
            return {
                .status = Render::EWindowSurfaceCreateStatus::UnsupportedRHI,
            };
        }
        if (window == nullptr || instance == nullptr || surface == nullptr) {
            return {
                .status = Render::EWindowSurfaceCreateStatus::InvalidSource,
            };
        }

        const VkResult result = glfwCreateWindowSurface(
            static_cast<VkInstance>(instance),
            window,
            static_cast<const VkAllocationCallbacks*>(allocation_callbacks),
            static_cast<VkSurfaceKHR*>(surface)
        );
        return {
            .status            = result == VK_SUCCESS ? Render::EWindowSurfaceCreateStatus::Success :
                                                        Render::EWindowSurfaceCreateStatus::NativeFailure,
            .native_error_code = static_cast<int64_t>(result),
        };
    }

private:
    Render::WindowSurfaceIdentity          identity{};
    GLFWwindow*                            window{nullptr};
    SharedPtr<GLFWWindowSurfaceLeaseState> lease_state;
};

} // namespace

GLFWWindowImpl::GLFWWindowImpl() : surface_lease_state(MakeShared<GLFWWindowSurfaceLeaseState>()) {}
GLFWWindowImpl::~GLFWWindowImpl() {}
void GLFWWindowImpl::PollEvents() const {
    glfwPollEvents();
}

void GLFWWindowImpl::Init(const SurfaceInitInfo& info) {
    if (!glfwInit()) {
        LOG_ERROR("Window init fail.");
        throw std::runtime_error("GLFW initialization failed");
    }
    GLFWInitTransaction init_transaction(main_window_handle.window);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
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
        if (fullscreen_monitor == nullptr) {
            throw std::runtime_error("No primary monitor is available for fullscreen window creation");
        }
        const GLFWvidmode* mode               = glfwGetVideoMode(fullscreen_monitor);
        if (mode == nullptr) {
            throw std::runtime_error("Failed to query the primary monitor video mode");
        }
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
    init_transaction.Adopt(window);
    // Window hints persist across creations. Restore the normal default so editor platform
    // windows are not accidentally created hidden after a hidden main-window bootstrap.
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (window == nullptr) {
        const char* description = nullptr;
        const int   error       = glfwGetError(&description);
        LOG_ERROR(
            "Failed to create GLFW window: error={}, description={}",
            error,
            description ? description : "<none>"
        );
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwSetWindowUserPointer(window, this);

    this->main_window_handle.window = window;

    init_transaction.Commit();
}

void GLFWWindowImpl::Tick() {
    PollEvents();
}
void GLFWWindowImpl::ShutDown() {
    auto* window = static_cast<GLFWwindow*>(main_window_handle.window);
    if (window == nullptr) {
        return;
    }

    const uint32_t live_surface_leases =
        surface_lease_state->live_surface_leases.load(std::memory_order_acquire);
    if (live_surface_leases != 0) {
        LOG_ERROR(
            "Refusing to destroy the GLFW window while {} swapchain surface source lease(s) remain.",
            live_surface_leases
        );
        std::terminate();
    }

    {
        std::scoped_lock lock(surface_source_mutex);
        surface_sources.clear();
    }
    glfwDestroyWindow(window);
    main_window_handle.window = nullptr;
    glfwTerminate();
}

void GLFWWindowImpl::SetCursorHide(WindowHandle* window) {
    auto* glfw_window = static_cast<GLFWwindow*>(window->window);
    glfwSetInputMode(glfw_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
#ifdef GLFW_RAW_MOUSE_MOTION
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(glfw_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
#endif
}

void GLFWWindowImpl::SetCursorNormal(WindowHandle* window) {
    auto* glfw_window = static_cast<GLFWwindow*>(window->window);
#ifdef GLFW_RAW_MOUSE_MOTION
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(glfw_window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
#endif
    glfwSetInputMode(glfw_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void GLFWWindowImpl::ApplyCursorMode(WindowHandle* window, bool hidden) {
    if (!IsCurrentlyGameThread()) {
        LOG_ERROR("GLFW cursor mode may only be changed on the Game Thread.");
        return;
    }
    if (window == nullptr || window->window == nullptr) {
        return;
    }

    auto*      glfw_window = static_cast<GLFWwindow*>(window->window);
    const bool is_hidden = glfwGetInputMode(glfw_window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
    if (is_hidden == hidden) {
        return;
    }

    if (hidden) {
        SetCursorHide(window);
    } else {
        SetCursorNormal(window);
    }
}

void GLFWWindowImpl::SetFocusMode(WindowHandle* _window, bool _focused) {
    ApplyCursorMode(_window, _focused);
}

bool GLFWWindowImpl::GetFocusMode(WindowHandle* window) const {
    if (!IsCurrentlyGameThread()) {
        LOG_ERROR("GLFW cursor mode may only be queried on the Game Thread.");
        return false;
    }
    if (window == nullptr || window->window == nullptr) {
        return false;
    }
    return glfwGetInputMode(static_cast<GLFWwindow*>(window->window), GLFW_CURSOR) ==
           GLFW_CURSOR_DISABLED;
}

Render::WindowFrameMetrics
GLFWWindowImpl::CaptureWindowFrameMetrics(const WindowHandle& window) const {
    if (!IsCurrentlyGameThread()) {
        LOG_ERROR("Window frame metrics may only be captured on the Game Thread.");
        return {};
    }

    auto* glfw_window = static_cast<GLFWwindow*>(window.window);
    if (glfw_window == nullptr) {
        return {};
    }

    int logical_width   = 0;
    int logical_height  = 0;
    int drawable_width  = 0;
    int drawable_height = 0;
    glfwGetWindowSize(glfw_window, &logical_width, &logical_height);
    glfwGetFramebufferSize(glfw_window, &drawable_width, &drawable_height);
    if (logical_width < 0 || logical_height < 0 || drawable_width < 0 || drawable_height < 0) {
        LOG_ERROR(
            "GLFW returned invalid window metrics: logical={}x{}, drawable={}x{}.",
            logical_width,
            logical_height,
            drawable_width,
            drawable_height
        );
        return {};
    }

    return Render::WindowFrameMetrics{
        .logical_extent =
            Extent2D(static_cast<uint32_t>(logical_width), static_cast<uint32_t>(logical_height)),
        .drawable_extent = Extent2D(
            static_cast<uint32_t>(drawable_width), static_cast<uint32_t>(drawable_height)
        ),
        .valid = true
    };
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
    MOER_ASSERT(
        IsCurrentlyGameThread(),
        "GLFW main-window visibility must be changed on the game thread"
    );
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
Render::SwapchainSurfaceInfo GLFWWindowImpl::CreateSwapchainSurfaceInfo(const WindowHandle& window) const {
    if (!IsCurrentlyGameThread()) {
        LOG_ERROR("Window surface sources may only be captured on the Game Thread.");
        return {};
    }
    auto* glfw_window = static_cast<GLFWwindow*>(window.window);
    if (glfw_window == nullptr) {
        return {};
    }

    std::scoped_lock lock(surface_source_mutex);
    auto&            weak_source = surface_sources[window.window];
    if (auto source = weak_source.lock()) {
        return {.source = std::move(source)};
    }

    uintptr_t platform_window_handle = 0;
#if PLATFORM_WINDOWS
    platform_window_handle = reinterpret_cast<uintptr_t>(glfwGetWin32Window(glfw_window));
#endif

    const uint64_t generation = next_surface_generation.fetch_add(1, std::memory_order_relaxed);
    auto           source     = MakeShared<GLFWWindowSurfaceSource>(
        glfw_window, platform_window_handle, generation, surface_lease_state
    );
    weak_source = source;
    return {.source = std::move(source)};
}

} // namespace Moer
