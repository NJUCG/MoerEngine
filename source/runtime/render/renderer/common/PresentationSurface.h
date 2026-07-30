#pragma once

#include "RenderAPI.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutorBackend.h"
#include "window/WindowFrameSnapshot.h"

#include <optional>
#include <string>

namespace Moer::Render {

struct PresentationSurfaceSnapshot {
    WindowSurfaceIdentity surface_identity{};
    Extent2D              source_drawable_extent{};
    Extent2D              drawable_extent{};
    uint64_t              capture_sequence    = 0;
    uint64_t              drawable_generation = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return surface_identity.IsValid() && drawable_extent.x != 0 && drawable_extent.y != 0 &&
               drawable_generation != 0;
    }
};

enum class EPresentationSurfacePlan : uint8_t {
    Invalid,
    MetadataOnly,
    Reuse,
    Refresh,
};

// Pure reducer for the presentation transaction. Native swapchain and
// framebuffer work is performed by PresentationSurface, while this state
// freezes the identity/generation that was committed successfully.
class RENDER_API PresentationSurfaceState {
public:
    [[nodiscard]] EPresentationSurfacePlan Plan(
        const SwapchainSurfaceInfo& surface,
        const WindowFrameSnapshot&  window_frame,
        bool                        native_commit_current
    ) noexcept;

    void RequestRefresh() noexcept {
        refresh_pending_ = true;
    }

    [[nodiscard]] bool Commit(const WindowFrameSnapshot& window_frame, Extent2D actual_extent) noexcept;

    [[nodiscard]] bool ApplyPresentFeedback(const PresentReceiptResult& feedback) noexcept;

    void Reject() noexcept {
        refresh_pending_ = true;
    }

    void Reset() noexcept {
        committed_        = {};
        last_observed_    = {};
        committed_epoch_  = 0;
        refresh_pending_  = true;
    }

    [[nodiscard]] bool
    IsCurrent(const WindowFrameSnapshot& window_frame, bool native_commit_current) const noexcept;

    [[nodiscard]] const PresentationSurfaceSnapshot& GetCommittedSnapshot() const noexcept {
        return committed_;
    }

    [[nodiscard]] bool HasRefreshPending() const noexcept {
        return refresh_pending_;
    }

    [[nodiscard]] uint64 GetCommittedEpoch() const noexcept {
        return committed_epoch_;
    }

private:
    [[nodiscard]] bool CanObserve(const WindowFrameSnapshot& window_frame) const noexcept;
    void               Observe(const WindowFrameSnapshot& window_frame) noexcept;

    PresentationSurfaceSnapshot committed_{};
    WindowFrameSnapshot         last_observed_{};
    uint64                      committed_epoch_{0};
    uint64                      next_epoch_{1};
    bool                        refresh_pending_{true};
};

struct PresentationSurfaceFrameBufferDesc {
    EPixelFormat       format{PF_R8G8B8A8_SRGB};
    ETextureUsageFlags usage{ETextureUsageFlags::COLOR_ATTACHMENT};
};

struct PresentationSurfaceDesc {
    uint                                              back_buffer_count{2};
    EPixelFormat                                      preferred_format{PF_R8G8B8A8_SRGB};
    std::string                                       debug_name{"PresentationSurface"};
    std::optional<PresentationSurfaceFrameBufferDesc> frame_buffer{};
    PresentFeedbackMailboxRef                         feedback_mailbox{};
};

enum class EPresentationSurfaceEnsureResult : uint8_t {
    Invalid,
    MetadataOnly,
    Current,
    Committed,
    RetryPending,
};

[[nodiscard]] constexpr bool IsPresentationSurfaceReady(EPresentationSurfaceEnsureResult result) noexcept {
    return result == EPresentationSurfaceEnsureResult::Current ||
           result == EPresentationSurfaceEnsureResult::Committed;
}

// Render-owner presentation object. The Game Thread supplies immutable surface
// and window snapshots; all native creation, recreation, quiesce and release
// work remains on the Render owner.
class RENDER_API PresentationSurface final {
public:
    PresentationSurface(RenderDevice& device, PresentationSurfaceDesc desc);
    ~PresentationSurface();

    PresentationSurface(const PresentationSurface&)            = delete;
    PresentationSurface& operator=(const PresentationSurface&) = delete;
    PresentationSurface(PresentationSurface&&)                 = delete;
    PresentationSurface& operator=(PresentationSurface&&)      = delete;

    [[nodiscard]] EPresentationSurfaceEnsureResult EnsureCurrent(
        const SwapchainSurfaceInfo& surface,
        const WindowFrameSnapshot&  window_frame,
        bool                        force_refresh = false
    );

    [[nodiscard]] std::optional<RHIPresentRequest> CreatePresentRequest(
        const WindowFrameSnapshot& window_frame,
        TextureView                source,
        PresentReceiptRef*         out_receipt = nullptr
    );

    void Quiesce();
    void Release();

    [[nodiscard]] bool IsCurrent(const WindowFrameSnapshot& window_frame) const noexcept;
    [[nodiscard]] bool IsPresentable() const noexcept;

    [[nodiscard]] SwapchainRef GetSwapchain() const {
        return swapchain_;
    }

    [[nodiscard]] TextureRef GetFrameBuffer() const {
        return frame_buffer_;
    }

    [[nodiscard]] TextureView GetFrameBufferView() const {
        return frame_buffer_ ? frame_buffer_->GetView() : TextureView{};
    }

    [[nodiscard]] EPixelFormat GetFormat() const noexcept {
        return swapchain_ ? swapchain_->format : desc_.preferred_format;
    }

    [[nodiscard]] Extent2D GetExtent() const noexcept {
        return state_.GetCommittedSnapshot().drawable_extent;
    }

    [[nodiscard]] const PresentationSurfaceSnapshot& GetCommittedSnapshot() const noexcept {
        return state_.GetCommittedSnapshot();
    }

    [[nodiscard]] bool HasRefreshPending() const noexcept {
        return state_.HasRefreshPending();
    }

private:
    [[nodiscard]] bool NativeCommitCurrent() const noexcept;
    [[nodiscard]] bool FrameBufferMatches(Extent2D extent) const noexcept;
    void               AssertRenderOwner() const noexcept;

    RenderDevice&            device_;
    PresentationSurfaceDesc  desc_;
    PresentationSurfaceState  state_{};
    SwapchainRef              swapchain_{};
    TextureRef                frame_buffer_{};
    PresentFeedbackMailboxRef feedback_mailbox_{};
    uint64                     next_present_request_serial_{1};
    bool                       force_surface_recreate_pending_{false};
};

} // namespace Moer::Render
