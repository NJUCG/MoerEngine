#pragma once

#include "RenderAPI.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"

#include <cstdint>

namespace Moer::Render {

enum class EWindowSystemType : uint8_t {
    Unknown,
    GLFW,
};

struct WindowSurfaceIdentity {
    EWindowSystemType window_system{EWindowSystemType::Unknown};
    uintptr_t         window_system_handle{0};
    uintptr_t         platform_window_handle{0};
    uint64_t          generation{0};

    [[nodiscard]] bool IsValid() const noexcept {
        return window_system != EWindowSystemType::Unknown && window_system_handle != 0 && generation != 0;
    }

    friend bool operator==(const WindowSurfaceIdentity&, const WindowSurfaceIdentity&) = default;
};

// Borrowed handles for a window whose WindowSurfaceSource lease is retained.
// window_system_handle is the window-library object (GLFWwindow* for GLFW);
// platform_window_handle is optional (HWND on Windows). Backends create and
// destroy their own presentation objects from these handles.
struct WindowNativeHandle {
    EWindowSystemType window_system{EWindowSystemType::Unknown};
    uintptr_t         window_system_handle{0};
    uintptr_t         platform_window_handle{0};

    [[nodiscard]] bool IsValid() const noexcept {
        return window_system != EWindowSystemType::Unknown && window_system_handle != 0;
    }
};

// Immutable lease over a window owned by the platform window layer. Keep the
// source alive while a backend uses its borrowed native handle. The native
// handle must describe the same window as the identity. Identity is for
// detecting window replacement; it is not a presentation API handle.
class RENDER_API WindowSurfaceSource {
public:
    virtual ~WindowSurfaceSource() = default;

    [[nodiscard]] virtual WindowSurfaceIdentity GetIdentity() const noexcept = 0;
    [[nodiscard]] virtual WindowNativeHandle GetNativeWindow() const noexcept = 0;
};

using WindowSurfaceSourceRef = SharedPtr<const WindowSurfaceSource>;

struct SwapchainSurfaceInfo {
    WindowSurfaceSourceRef source;

    [[nodiscard]] WindowSurfaceIdentity GetIdentity() const noexcept {
        return source ? source->GetIdentity() : WindowSurfaceIdentity{};
    }

    [[nodiscard]] bool IsValid() const noexcept {
        if (!source) {
            return false;
        }
        const WindowSurfaceIdentity identity = source->GetIdentity();
        const WindowNativeHandle    native   = source->GetNativeWindow();
        return identity.IsValid() && native.IsValid() && identity.window_system == native.window_system &&
               identity.window_system_handle == native.window_system_handle &&
               identity.platform_window_handle == native.platform_window_handle;
    }

    [[nodiscard]] bool HasSameIdentity(const SwapchainSurfaceInfo& other) const noexcept {
        return IsValid() && other.IsValid() && GetIdentity() == other.GetIdentity();
    }
};

enum class ESwapchainSurfaceTransition : uint8_t {
    Invalid,
    Initialize,
    Reuse,
    Replace,
};

[[nodiscard]] inline ESwapchainSurfaceTransition ClassifySwapchainSurfaceTransition(
    const SwapchainSurfaceInfo& committed,
    bool                        has_committed_surface,
    const SwapchainSurfaceInfo& incoming
) noexcept {
    if (!incoming.IsValid()) {
        return ESwapchainSurfaceTransition::Invalid;
    }
    if (!has_committed_surface) {
        return ESwapchainSurfaceTransition::Initialize;
    }
    return committed.HasSameIdentity(incoming) ? ESwapchainSurfaceTransition::Reuse :
                                                 ESwapchainSurfaceTransition::Replace;
}

} // namespace Moer::Render
