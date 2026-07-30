#pragma once

#include "rhi/RHIWindowSurface.h"

#include <cstdint>

namespace Moer::Render {

struct WindowFrameMetrics {
    Extent2D logical_extent{};
    Extent2D drawable_extent{};
    bool     valid{false};
};

enum class EWindowFrameTransition : uint8_t {
    Invalid,
    Initial,
    Stable,
    LogicalResized,
    DrawableResized,
    Minimized,
    Restored,
    Replaced,
};

struct WindowFrameSnapshot {
    WindowSurfaceIdentity   surface_identity{};
    Extent2D                logical_extent{};
    Extent2D                drawable_extent{};
    Extent2D                last_drawable_extent{};
    uint64_t                capture_sequence{0};
    uint64_t                drawable_generation{0};
    EWindowFrameTransition  transition{EWindowFrameTransition::Invalid};

    [[nodiscard]] bool IsValid() const noexcept {
        return transition != EWindowFrameTransition::Invalid && surface_identity.IsValid();
    }

    [[nodiscard]] bool IsDrawable() const noexcept {
        return IsValid() && drawable_extent.width != 0 && drawable_extent.height != 0;
    }

    [[nodiscard]] Extent2D GetStableDrawableExtent() const noexcept {
        return last_drawable_extent;
    }

    [[nodiscard]] Moer::uint2 GetStableDrawableResolution() const noexcept {
        return {last_drawable_extent.x, last_drawable_extent.y};
    }
};

// Pure state machine for turning GT-captured window metrics into immutable
// frame values. Invalid captures advance sequencing but never replace the last
// valid state used to classify the next capture.
class WindowFrameSnapshotTracker {
public:
    [[nodiscard]] WindowFrameSnapshot
    Advance(const WindowSurfaceIdentity& surface_identity, const WindowFrameMetrics& metrics) noexcept {
        const uint64_t capture_sequence = ++capture_sequence_;
        if (!surface_identity.IsValid() || !metrics.valid) {
            WindowFrameSnapshot invalid_snapshot = last_valid_snapshot_;
            invalid_snapshot.capture_sequence    = capture_sequence;
            invalid_snapshot.transition          = EWindowFrameTransition::Invalid;
            return invalid_snapshot;
        }

        const bool has_drawable =
            metrics.drawable_extent.width != 0 && metrics.drawable_extent.height != 0;

        WindowFrameSnapshot snapshot{
            .surface_identity     = surface_identity,
            .logical_extent       = metrics.logical_extent,
            .drawable_extent      = metrics.drawable_extent,
            .capture_sequence     = capture_sequence,
        };

        if (!has_valid_snapshot_) {
            snapshot.last_drawable_extent = has_drawable ? metrics.drawable_extent : Extent2D{};
            snapshot.drawable_generation  = has_drawable ? 1 : 0;
            snapshot.transition =
                has_drawable ? EWindowFrameTransition::Initial : EWindowFrameTransition::Minimized;
            Commit(snapshot);
            return snapshot;
        }

        if (surface_identity != last_valid_snapshot_.surface_identity) {
            snapshot.last_drawable_extent = has_drawable ? metrics.drawable_extent : Extent2D{};
            snapshot.drawable_generation  = last_valid_snapshot_.drawable_generation + 1;
            snapshot.transition           = EWindowFrameTransition::Replaced;
            Commit(snapshot);
            return snapshot;
        }

        const bool had_drawable = last_valid_snapshot_.IsDrawable();
        snapshot.last_drawable_extent = last_valid_snapshot_.last_drawable_extent;
        snapshot.drawable_generation  = last_valid_snapshot_.drawable_generation;

        if (!has_drawable) {
            snapshot.transition =
                had_drawable ? EWindowFrameTransition::Minimized : EWindowFrameTransition::Stable;
        } else if (!had_drawable) {
            snapshot.last_drawable_extent = metrics.drawable_extent;
            ++snapshot.drawable_generation;
            snapshot.transition = EWindowFrameTransition::Restored;
        } else if (metrics.drawable_extent != last_valid_snapshot_.drawable_extent) {
            snapshot.last_drawable_extent = metrics.drawable_extent;
            ++snapshot.drawable_generation;
            snapshot.transition = EWindowFrameTransition::DrawableResized;
        } else if (metrics.logical_extent != last_valid_snapshot_.logical_extent) {
            snapshot.last_drawable_extent = metrics.drawable_extent;
            snapshot.transition           = EWindowFrameTransition::LogicalResized;
        } else {
            snapshot.last_drawable_extent = metrics.drawable_extent;
            snapshot.transition           = EWindowFrameTransition::Stable;
        }

        Commit(snapshot);
        return snapshot;
    }

private:
    void Commit(const WindowFrameSnapshot& snapshot) noexcept {
        last_valid_snapshot_ = snapshot;
        has_valid_snapshot_  = true;
    }

    WindowFrameSnapshot last_valid_snapshot_{};
    uint64_t            capture_sequence_{0};
    bool                has_valid_snapshot_{false};
};

} // namespace Moer::Render
