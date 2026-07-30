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

enum class EWindowSurfaceCreateStatus : uint8_t {
    Success,
    InvalidSource,
    UnsupportedRHI,
    NativeFailure,
};

struct WindowSurfaceCreateResult {
    EWindowSurfaceCreateStatus status{EWindowSurfaceCreateStatus::InvalidSource};
    int64_t                    native_error_code{0};

    [[nodiscard]] bool Succeeded() const noexcept {
        return status == EWindowSurfaceCreateStatus::Success;
    }
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

// Immutable, non-owning lease over a native window. The platform window layer
// owns native-window creation/destruction; swapchains retain this object only
// to keep a stable identity and surface factory across render-thread work.
class RENDER_API WindowSurfaceSource {
public:
    virtual ~WindowSurfaceSource() = default;

    [[nodiscard]] virtual WindowSurfaceIdentity GetIdentity() const noexcept = 0;
    [[nodiscard]] virtual WindowSurfaceCreateResult
    CreateSurface(ERHIType rhi_type, void* instance, const void* allocation_callbacks, void* surface)
        const noexcept = 0;
};

using WindowSurfaceSourceRef = SharedPtr<const WindowSurfaceSource>;

struct SwapchainSurfaceInfo {
    WindowSurfaceSourceRef source;

    [[nodiscard]] WindowSurfaceIdentity GetIdentity() const noexcept {
        return source ? source->GetIdentity() : WindowSurfaceIdentity{};
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return GetIdentity().IsValid();
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
