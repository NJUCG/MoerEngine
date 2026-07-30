#include "renderer/common/PresentationSurface.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace Moer::Render;

namespace {

void RequireAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "PresentationSurfaceContract failed at line %d\n", line);
        std::_Exit(EXIT_FAILURE);
    }
}

#define Require(condition) RequireAt((condition), __LINE__)

class FakeWindowSurfaceSource final : public WindowSurfaceSource {
public:
    explicit FakeWindowSurfaceSource(WindowSurfaceIdentity identity) : identity_(identity) {}

    [[nodiscard]] WindowSurfaceIdentity GetIdentity() const noexcept override {
        return identity_;
    }

    [[nodiscard]] WindowSurfaceCreateResult
    CreateSurface(ERHIType, void*, const void*, void*) const noexcept override {
        return {
            .status = EWindowSurfaceCreateStatus::UnsupportedRHI,
        };
    }

private:
    WindowSurfaceIdentity identity_{};
};

WindowSurfaceIdentity
MakeIdentity(uintptr_t window_system_handle, uintptr_t platform_window_handle, uint64_t generation) {
    return {
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = window_system_handle,
        .platform_window_handle = platform_window_handle,
        .generation             = generation,
    };
}

SwapchainSurfaceInfo MakeSurface(WindowSurfaceIdentity identity) {
    return {
        std::make_shared<FakeWindowSurfaceSource>(identity),
    };
}

WindowFrameMetrics MakeMetrics(Extent2D logical_extent, Extent2D drawable_extent) {
    return {
        .logical_extent  = logical_extent,
        .drawable_extent = drawable_extent,
        .valid           = true,
    };
}

} // namespace

int main() {
    const WindowSurfaceIdentity identity_a = MakeIdentity(0x1000u, 0x2000u, 1u);
    const SwapchainSurfaceInfo  surface_a  = MakeSurface(identity_a);

    PresentationSurfaceState   state;
    WindowFrameSnapshotTracker tracker;
    const WindowFrameSnapshot  initial = tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));

    Require(state.Plan({}, initial, false) == EPresentationSurfacePlan::Invalid);
    Require(state.Plan(surface_a, initial, false) == EPresentationSurfacePlan::Refresh);
    Require(state.HasRefreshPending());
    Require(state.Commit(initial, {1024, 768}));
    Require(!state.HasRefreshPending());
    Require(state.GetCommittedSnapshot().IsValid());
    Require(state.GetCommittedSnapshot().source_drawable_extent == Extent2D(800, 600));
    Require(state.GetCommittedSnapshot().drawable_extent == Extent2D(1024, 768));
    Require(state.Plan(surface_a, initial, true) == EPresentationSurfacePlan::Reuse);

    const WindowFrameSnapshot stable = tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    Require(state.IsCurrent(stable, true));
    Require(state.Plan(surface_a, stable, true) == EPresentationSurfacePlan::Reuse);

    const WindowFrameSnapshot logical_resized =
        tracker.Advance(identity_a, MakeMetrics({640, 480}, {800, 600}));
    Require(state.IsCurrent(logical_resized, true));
    Require(state.Plan(surface_a, logical_resized, true) == EPresentationSurfacePlan::Reuse);

    WindowFrameSnapshot inconsistent_extent = logical_resized;
    inconsistent_extent.drawable_extent     = {801, 600};
    Require(state.Plan(surface_a, inconsistent_extent, true) == EPresentationSurfacePlan::Invalid);
    Require(state.Plan(surface_a, logical_resized, false) == EPresentationSurfacePlan::Refresh);

    state.RequestRefresh();
    Require(state.HasRefreshPending());
    Require(state.Plan(surface_a, logical_resized, true) == EPresentationSurfacePlan::Refresh);

    const WindowFrameSnapshot minimized = tracker.Advance(identity_a, MakeMetrics({640, 480}, {0, 0}));
    Require(state.Plan(surface_a, minimized, true) == EPresentationSurfacePlan::MetadataOnly);
    Require(state.HasRefreshPending());

    const WindowFrameSnapshot restored = tracker.Advance(identity_a, MakeMetrics({640, 480}, {800, 600}));
    Require(restored.drawable_generation > initial.drawable_generation);
    Require(state.Plan(surface_a, restored, true) == EPresentationSurfacePlan::Refresh);
    Require(state.Commit(restored, {800, 600}));
    Require(state.IsCurrent(restored, true));
    Require(state.Plan(surface_a, initial, true) == EPresentationSurfacePlan::Invalid);

    const WindowFrameSnapshot drawable_resized =
        tracker.Advance(identity_a, MakeMetrics({640, 480}, {1280, 960}));
    Require(state.Plan(surface_a, drawable_resized, true) == EPresentationSurfacePlan::Refresh);
    state.Reject();
    Require(state.HasRefreshPending());
    Require(state.GetCommittedSnapshot().drawable_generation == restored.drawable_generation);
    Require(state.Plan(surface_a, drawable_resized, true) == EPresentationSurfacePlan::Refresh);
    Require(state.Commit(drawable_resized, {1278, 958}));
    Require(state.GetCommittedSnapshot().drawable_extent == Extent2D(1278, 958));

    const WindowSurfaceIdentity identity_b = MakeIdentity(0x3000u, 0x4000u, 1u);
    const SwapchainSurfaceInfo  surface_b  = MakeSurface(identity_b);
    const WindowFrameSnapshot   replaced = tracker.Advance(identity_b, MakeMetrics({900, 700}, {1800, 1400}));
    Require(state.Plan(surface_b, replaced, false) == EPresentationSurfacePlan::Refresh);
    Require(state.Commit(replaced, {1800, 1400}));
    Require(state.Plan(surface_a, replaced, true) == EPresentationSurfacePlan::Invalid);

    const WindowSurfaceIdentity identity_a2 = MakeIdentity(0x1000u, 0x2000u, 2u);
    const SwapchainSurfaceInfo  surface_a2  = MakeSurface(identity_a2);
    const WindowFrameSnapshot   aba_replaced =
        tracker.Advance(identity_a2, MakeMetrics({800, 600}, {800, 600}));
    Require(state.Plan(surface_a2, aba_replaced, false) == EPresentationSurfacePlan::Refresh);

    PresentationSurfaceState   zero_first_state;
    WindowFrameSnapshotTracker zero_first_tracker;
    const WindowFrameSnapshot  zero_first =
        zero_first_tracker.Advance(identity_a2, MakeMetrics({800, 600}, {0, 0}));
    Require(zero_first.drawable_generation == 0);
    Require(zero_first_state.Plan(surface_a2, zero_first, false) == EPresentationSurfacePlan::MetadataOnly);
    const WindowFrameSnapshot zero_first_restored =
        zero_first_tracker.Advance(identity_a2, MakeMetrics({800, 600}, {800, 600}));
    Require(
        zero_first_state.Plan(surface_a2, zero_first_restored, false) == EPresentationSurfacePlan::Refresh
    );

    PresentationSurfaceState   retry_state;
    WindowFrameSnapshotTracker retry_tracker;
    const WindowFrameSnapshot  retry_initial =
        retry_tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    Require(retry_state.Plan(surface_a, retry_initial, false) == EPresentationSurfacePlan::Refresh);
    Require(retry_state.Commit(retry_initial, {800, 600}));
    const WindowFrameSnapshot delayed_stable =
        retry_tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    const WindowFrameSnapshot failed_resize =
        retry_tracker.Advance(identity_a, MakeMetrics({960, 720}, {960, 720}));
    Require(retry_state.Plan(surface_a, failed_resize, true) == EPresentationSurfacePlan::Refresh);
    retry_state.Reject();
    Require(retry_state.Plan(surface_a, delayed_stable, true) == EPresentationSurfacePlan::Invalid);
    Require(retry_state.Plan(surface_a, failed_resize, true) == EPresentationSurfacePlan::Refresh);

    state.Reset();
    Require(state.HasRefreshPending());
    Require(!state.GetCommittedSnapshot().IsValid());
    Require(state.Plan(surface_a2, aba_replaced, false) == EPresentationSurfacePlan::Refresh);

    return 0;
}
