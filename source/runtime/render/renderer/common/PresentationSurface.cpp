#include "renderer/common/PresentationSurface.h"

#include "RenderThread.h"
#include "rhi/RHIExecutor.h"

#include <cassert>
#include <exception>

namespace Moer::Render {

EPresentationSurfacePlan PresentationSurfaceState::Plan(
    const SwapchainSurfaceInfo& surface,
    const WindowFrameSnapshot&  window_frame,
    bool                        native_commit_current
) noexcept {
    if (!surface.IsValid() || !window_frame.IsValid() ||
        surface.GetIdentity() != window_frame.surface_identity || window_frame.capture_sequence == 0) {
        return EPresentationSurfacePlan::Invalid;
    }
    if (!CanObserve(window_frame)) {
        return EPresentationSurfacePlan::Invalid;
    }
    if (committed_.IsValid()) {
        if (window_frame.capture_sequence < committed_.capture_sequence ||
            window_frame.drawable_generation < committed_.drawable_generation) {
            return EPresentationSurfacePlan::Invalid;
        }
    }
    if (!window_frame.IsDrawable()) {
        Observe(window_frame);
        return EPresentationSurfacePlan::MetadataOnly;
    }
    if (window_frame.drawable_generation == 0) {
        return EPresentationSurfacePlan::Invalid;
    }
    if (committed_.IsValid()) {
        if (committed_.surface_identity == window_frame.surface_identity &&
            committed_.drawable_generation == window_frame.drawable_generation &&
            committed_.source_drawable_extent != window_frame.drawable_extent) {
            return EPresentationSurfacePlan::Invalid;
        }
    }
    Observe(window_frame);
    return IsCurrent(window_frame, native_commit_current) ? EPresentationSurfacePlan::Reuse :
                                                            EPresentationSurfacePlan::Refresh;
}

bool PresentationSurfaceState::Commit(
    const WindowFrameSnapshot& window_frame,
    Extent2D                   actual_extent
) noexcept {
    if (!window_frame.IsDrawable() || window_frame.capture_sequence == 0 ||
        window_frame.drawable_generation == 0 || actual_extent.x == 0 || actual_extent.y == 0 ||
        !CanObserve(window_frame)) {
        Reject();
        return false;
    }

    Observe(window_frame);
    committed_ = PresentationSurfaceSnapshot{
        .surface_identity       = window_frame.surface_identity,
        .source_drawable_extent = window_frame.drawable_extent,
        .drawable_extent        = actual_extent,
        .capture_sequence       = window_frame.capture_sequence,
        .drawable_generation    = window_frame.drawable_generation,
    };
    committed_epoch_ = next_epoch_++;
    if (next_epoch_ == 0) {
        next_epoch_ = 1;
    }
    refresh_pending_ = false;
    return true;
}

bool PresentationSurfaceState::ApplyPresentFeedback(const PresentReceiptResult& feedback) noexcept {
    if (!feedback.resolved || !PresentStatusRequiresRefresh(feedback.status) ||
        feedback.context.presentation_epoch == 0 ||
        feedback.context.presentation_epoch != committed_epoch_ ||
        feedback.context.drawable_generation != committed_.drawable_generation) {
        return false;
    }
    refresh_pending_ = true;
    return true;
}

bool PresentationSurfaceState::CanObserve(const WindowFrameSnapshot& window_frame) const noexcept {
    if (!last_observed_.IsValid()) {
        return true;
    }
    if (window_frame.capture_sequence < last_observed_.capture_sequence ||
        window_frame.drawable_generation < last_observed_.drawable_generation) {
        return false;
    }
    if (window_frame.capture_sequence == last_observed_.capture_sequence) {
        return window_frame.surface_identity == last_observed_.surface_identity &&
               window_frame.logical_extent == last_observed_.logical_extent &&
               window_frame.drawable_extent == last_observed_.drawable_extent &&
               window_frame.last_drawable_extent == last_observed_.last_drawable_extent &&
               window_frame.drawable_generation == last_observed_.drawable_generation &&
               window_frame.transition == last_observed_.transition;
    }
    if (window_frame.surface_identity != last_observed_.surface_identity &&
        window_frame.drawable_generation <= last_observed_.drawable_generation) {
        return false;
    }
    return window_frame.surface_identity != last_observed_.surface_identity ||
           window_frame.drawable_generation != last_observed_.drawable_generation ||
           !window_frame.IsDrawable() || !last_observed_.IsDrawable() ||
           window_frame.drawable_extent == last_observed_.drawable_extent;
}

void PresentationSurfaceState::Observe(const WindowFrameSnapshot& window_frame) noexcept {
    last_observed_ = window_frame;
}

bool PresentationSurfaceState::IsCurrent(const WindowFrameSnapshot& window_frame, bool native_commit_current)
    const noexcept {
    return !refresh_pending_ && native_commit_current && committed_.IsValid() && window_frame.IsDrawable() &&
           window_frame.capture_sequence >= committed_.capture_sequence &&
           committed_.surface_identity == window_frame.surface_identity &&
           committed_.source_drawable_extent == window_frame.drawable_extent &&
           committed_.drawable_generation == window_frame.drawable_generation;
}

PresentationSurface::PresentationSurface(RenderDevice& device, PresentationSurfaceDesc desc) :
    device_(device),
    desc_(std::move(desc)),
    feedback_mailbox_(
        desc_.feedback_mailbox ? desc_.feedback_mailbox :
                                 MakeShared<PresentFeedbackMailbox>()
    ) {
    AssertRenderOwner();
}

PresentationSurface::~PresentationSurface() noexcept {
    // Explicit Release() is the normal shutdown path; keep RAII cleanup as an
    // exception-safe fallback for partially constructed renderer owners.
    if (swapchain_ || frame_buffer_) {
        Release();
    }
}

EPresentationSurfaceEnsureResult PresentationSurface::EnsureCurrent(
    const SwapchainSurfaceInfo& surface,
    const WindowFrameSnapshot&  window_frame,
    bool                        force_refresh
) {
    AssertRenderOwner();
    if (feedback_mailbox_) {
        if (const auto feedback = feedback_mailbox_->ConsumeLatestRecovery();
            feedback.has_value() && state_.ApplyPresentFeedback(*feedback)) {
            force_surface_recreate_pending_ =
                force_surface_recreate_pending_ ||
                PresentStatusRequiresNativeSurfaceRefresh(feedback->status);
        }
    }
    if (force_refresh) {
        state_.RequestRefresh();
    }

    const bool                     native_commit_current = NativeCommitCurrent();
    const EPresentationSurfacePlan plan = state_.Plan(surface, window_frame, native_commit_current);
    if (plan == EPresentationSurfacePlan::Invalid) {
        return EPresentationSurfaceEnsureResult::Invalid;
    }
    if (plan == EPresentationSurfacePlan::MetadataOnly) {
        if (!native_commit_current) {
            state_.RequestRefresh();
        }
        return EPresentationSurfaceEnsureResult::MetadataOnly;
    }
    if (plan == EPresentationSurfacePlan::Reuse) {
        return EPresentationSurfaceEnsureResult::Current;
    }

    state_.RequestRefresh();
    try {
        Quiesce();
    } catch (const std::exception& error) {
        LOG_ERROR(
            "[Presentation] targeted drain failed before recreate: {}",
            error.what()
        );
        state_.Reject();
        return EPresentationSurfaceEnsureResult::RetryPending;
    } catch (...) {
        LOG_ERROR(
            "[Presentation] targeted drain failed before recreate"
        );
        state_.Reject();
        return EPresentationSurfaceEnsureResult::RetryPending;
    }

    const SwapchainCreateInfo create_info{
        .surface          = surface,
        .size             = window_frame.drawable_extent,
        .back_buffer_sz   = desc_.back_buffer_count,
        .preferred_format = desc_.preferred_format,
        .force_surface_recreate = force_surface_recreate_pending_,
    };

    if (!swapchain_) {
        SwapchainRef candidate = device_.CreateSwapchain(create_info);
        if (!candidate || !candidate->IsPresentationReady() ||
            candidate->GetCommittedSurfaceIdentity() != window_frame.surface_identity) {
            state_.Reject();
            return EPresentationSurfaceEnsureResult::RetryPending;
        }
        swapchain_ = std::move(candidate);
    } else if (!swapchain_->Recreate(create_info)) {
        state_.Reject();
        return EPresentationSurfaceEnsureResult::RetryPending;
    }

    if (!swapchain_ || !swapchain_->IsPresentationReady() ||
        swapchain_->GetCommittedSurfaceIdentity() != window_frame.surface_identity ||
        swapchain_->size.x == 0 || swapchain_->size.y == 0) {
        state_.Reject();
        return EPresentationSurfaceEnsureResult::RetryPending;
    }

    const Extent2D actual_extent          = swapchain_->size;
    TextureRef     committed_frame_buffer = frame_buffer_;
    if (desc_.frame_buffer.has_value() && !FrameBufferMatches(actual_extent)) {
        const auto& frame_buffer_desc = *desc_.frame_buffer;
        committed_frame_buffer =
            device_.CreateTexture(actual_extent, frame_buffer_desc.format, frame_buffer_desc.usage);
        if (!committed_frame_buffer) {
            state_.Reject();
            return EPresentationSurfaceEnsureResult::RetryPending;
        }
        committed_frame_buffer->SetName(desc_.debug_name);
    }

    if (!state_.Commit(window_frame, actual_extent)) {
        return EPresentationSurfaceEnsureResult::RetryPending;
    }
    force_surface_recreate_pending_ = false;
    frame_buffer_ = std::move(committed_frame_buffer);
    return EPresentationSurfaceEnsureResult::Committed;
}

std::optional<RHIPresentRequest> PresentationSurface::CreatePresentRequest(
    const WindowFrameSnapshot& window_frame,
    TextureView                source,
    PresentReceiptRef*         out_receipt
) {
    AssertRenderOwner();
    if (out_receipt != nullptr) {
        *out_receipt = {};
    }
    if (!IsCurrent(window_frame) || source.GetTexture() == nullptr) {
        return std::nullopt;
    }

    const Extent2D committed_extent = state_.GetCommittedSnapshot().drawable_extent;
    if (source.extent.x != committed_extent.x || source.extent.y != committed_extent.y) {
        return std::nullopt;
    }
    uint64 request_serial = next_present_request_serial_++;
    if (request_serial == 0) {
        request_serial = next_present_request_serial_++;
    }
    PresentReceiptRef receipt = MakeShared<PresentReceipt>(
        PresentReceiptContext{
            .presentation_epoch  = state_.GetCommittedEpoch(),
            .drawable_generation = state_.GetCommittedSnapshot().drawable_generation,
            .request_serial      = request_serial,
        },
        feedback_mailbox_
    );
    if (out_receipt != nullptr) {
        *out_receipt = receipt;
    }
    return RHIPresentRequest(swapchain_, source, std::move(receipt));
}

void PresentationSurface::Quiesce() {
    AssertRenderOwner();
    if (!swapchain_ && !frame_buffer_) {
        return;
    }
    if (swapchain_) {
        const PresentationSurfaceSnapshot& committed =
            state_.GetCommittedSnapshot();
        RHIExecutor::Get().DrainPresentation(
            RHIPresentationDrainTarget{
                swapchain_,
                state_.GetCommittedEpoch(),
                committed.drawable_generation,
            }
        );
    } else {
        // Defensive partial-construction fallback. A committed presentation
        // surface always owns a swapchain; only a failed setup path can retain
        // a framebuffer without one.
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }
}

void PresentationSurface::Release() noexcept {
    AssertRenderOwner();
    const auto fallback_to_device_idle =
        [this](const char* _reason) noexcept {
            // Recreate must reject an unsafe transition, but teardown cannot
            // let a cancelled/faulted executor escape an owner destructor.
            // Device idle is the final lifetime-safe fallback because it
            // serializes directly with every native queue operation.
            try {
                LOG_ERROR(
                    "[Presentation] targeted drain failed during release; "
                    "falling back to device idle: {}",
                    _reason
                );
            } catch (...) {
            }
            try {
                device_.WaitIdle();
            } catch (...) {
                try {
                    LOG_ERROR(
                        "[Presentation] device-idle fallback failed during release"
                    );
                } catch (...) {
                }
            }
        };

    if (swapchain_ || frame_buffer_) {
        try {
            Quiesce();
        } catch (const std::exception& error) {
            fallback_to_device_idle(error.what());
        } catch (...) {
            fallback_to_device_idle("unknown exception");
        }
    }
    swapchain_    = nullptr;
    frame_buffer_ = nullptr;
    force_surface_recreate_pending_ = false;
    if (feedback_mailbox_) {
        feedback_mailbox_->Reset();
    }
    state_.Reset();
}

bool PresentationSurface::IsCurrent(const WindowFrameSnapshot& window_frame) const noexcept {
    return state_.IsCurrent(window_frame, NativeCommitCurrent());
}

bool PresentationSurface::IsPresentable() const noexcept {
    return !state_.HasRefreshPending() && NativeCommitCurrent();
}

bool PresentationSurface::NativeCommitCurrent() const noexcept {
    const PresentationSurfaceSnapshot& committed = state_.GetCommittedSnapshot();
    if (!committed.IsValid() || !swapchain_ || !swapchain_->IsPresentationReady() ||
        swapchain_->GetCommittedSurfaceIdentity() != committed.surface_identity ||
        swapchain_->size != committed.drawable_extent) {
        return false;
    }
    return !desc_.frame_buffer.has_value() || FrameBufferMatches(committed.drawable_extent);
}

bool PresentationSurface::FrameBufferMatches(Extent2D extent) const noexcept {
    if (!desc_.frame_buffer.has_value()) {
        return !frame_buffer_;
    }
    if (!frame_buffer_ || frame_buffer_->GetFormat() != desc_.frame_buffer->format) {
        return false;
    }
    const uint3 frame_buffer_extent = frame_buffer_->GetExtent();
    return frame_buffer_extent.x == extent.x && frame_buffer_extent.y == extent.y;
}

void PresentationSurface::AssertRenderOwner() const noexcept {
    assert(!IsRenderThreadRunning() || IsCurrentlyRenderThread());
}

} // namespace Moer::Render
