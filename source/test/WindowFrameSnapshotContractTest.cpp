#include "renderer/common/UIRenderer.h"
#include "window/WindowFrameSnapshot.h"

#include <cstdlib>

using namespace Moer::Render;

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

WindowSurfaceIdentity MakeIdentity(
    uintptr_t window_system_handle,
    uintptr_t platform_window_handle,
    uint64_t  generation
) {
    return {
        .window_system          = EWindowSystemType::GLFW,
        .window_system_handle   = window_system_handle,
        .platform_window_handle = platform_window_handle,
        .generation             = generation,
    };
}

WindowFrameMetrics MakeMetrics(Extent2D logical_extent, Extent2D drawable_extent) {
    return {
        .logical_extent  = logical_extent,
        .drawable_extent = drawable_extent,
        .valid           = true,
    };
}

void RequireExtent(Extent2D actual, Extent2D expected) {
    Require(actual == expected);
}

} // namespace

int main() {
    const WindowSurfaceIdentity identity_a = MakeIdentity(0x1000u, 0x2000u, 1u);

    WindowFrameSnapshotTracker tracker;
    const WindowFrameSnapshot invalid_first = tracker.Advance({}, MakeMetrics({800, 600}, {800, 600}));
    Require(invalid_first.transition == EWindowFrameTransition::Invalid);
    Require(invalid_first.capture_sequence == 1);
    Require(!invalid_first.IsValid());
    Require(!invalid_first.IsDrawable());

    const WindowFrameSnapshot initial = tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    Require(initial.transition == EWindowFrameTransition::Initial);
    Require(initial.capture_sequence == 2);
    Require(initial.drawable_generation == 1);
    Require(initial.IsValid());
    Require(initial.IsDrawable());
    RequireExtent(initial.GetStableDrawableExtent(), {800, 600});
    const Moer::uint2 initial_stable_resolution = initial.GetStableDrawableResolution();
    Require(initial_stable_resolution.x == 800);
    Require(initial_stable_resolution.y == 600);

    const WindowFrameSnapshot immutable_copy = initial;
    const WindowFrameSnapshot stable = tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    Require(stable.transition == EWindowFrameTransition::Stable);
    Require(stable.capture_sequence == 3);
    Require(stable.drawable_generation == 1);
    Require(immutable_copy.transition == EWindowFrameTransition::Initial);
    Require(immutable_copy.capture_sequence == 2);
    RequireExtent(immutable_copy.drawable_extent, {800, 600});

    const WindowFrameSnapshot logical_resized =
        tracker.Advance(identity_a, MakeMetrics({640, 480}, {800, 600}));
    Require(logical_resized.transition == EWindowFrameTransition::LogicalResized);
    Require(logical_resized.capture_sequence == 4);
    Require(logical_resized.drawable_generation == 1);
    RequireExtent(logical_resized.logical_extent, {640, 480});
    RequireExtent(logical_resized.drawable_extent, {800, 600});

    // A DPI/content-scale change can leave the logical extent untouched while
    // changing the framebuffer extent.
    const WindowFrameSnapshot dpi_resized =
        tracker.Advance(identity_a, MakeMetrics({640, 480}, {1280, 960}));
    Require(dpi_resized.transition == EWindowFrameTransition::DrawableResized);
    Require(dpi_resized.capture_sequence == 5);
    Require(dpi_resized.drawable_generation == 2);
    RequireExtent(dpi_resized.GetStableDrawableExtent(), {1280, 960});

    const WindowFrameSnapshot minimized = tracker.Advance(identity_a, MakeMetrics({640, 480}, {0, 0}));
    Require(minimized.transition == EWindowFrameTransition::Minimized);
    Require(minimized.capture_sequence == 6);
    Require(minimized.drawable_generation == 2);
    Require(minimized.IsValid());
    Require(!minimized.IsDrawable());
    RequireExtent(minimized.GetStableDrawableExtent(), {1280, 960});

    const WindowFrameSnapshot repeated_zero =
        tracker.Advance(identity_a, MakeMetrics({320, 240}, {0, 0}));
    Require(repeated_zero.transition == EWindowFrameTransition::Stable);
    Require(repeated_zero.capture_sequence == 7);
    Require(repeated_zero.drawable_generation == 2);
    RequireExtent(repeated_zero.logical_extent, {320, 240});
    RequireExtent(repeated_zero.GetStableDrawableExtent(), {1280, 960});

    const WindowFrameSnapshot restored_same =
        tracker.Advance(identity_a, MakeMetrics({1280, 960}, {1280, 960}));
    Require(restored_same.transition == EWindowFrameTransition::Restored);
    Require(restored_same.capture_sequence == 8);
    Require(restored_same.drawable_generation == 3);
    Require(restored_same.IsDrawable());
    RequireExtent(restored_same.GetStableDrawableExtent(), {1280, 960});

    const WindowFrameSnapshot minimized_again =
        tracker.Advance(identity_a, MakeMetrics({1280, 960}, {0, 0}));
    Require(minimized_again.transition == EWindowFrameTransition::Minimized);
    Require(minimized_again.drawable_generation == 3);

    const WindowFrameSnapshot restored_different =
        tracker.Advance(identity_a, MakeMetrics({1024, 768}, {1024, 768}));
    Require(restored_different.transition == EWindowFrameTransition::Restored);
    Require(restored_different.capture_sequence == 10);
    Require(restored_different.drawable_generation == 4);
    RequireExtent(restored_different.GetStableDrawableExtent(), {1024, 768});

    const WindowFrameSnapshot drawable_resized =
        tracker.Advance(identity_a, MakeMetrics({1024, 768}, {1600, 1200}));
    Require(drawable_resized.transition == EWindowFrameTransition::DrawableResized);
    Require(drawable_resized.capture_sequence == 11);
    Require(drawable_resized.drawable_generation == 5);

    const WindowFrameSnapshot invalid_metrics = tracker.Advance(
        MakeIdentity(0x3000u, 0x4000u, 1u),
        {
            .logical_extent  = {1, 1},
            .drawable_extent = {1, 1},
            .valid           = false,
        }
    );
    Require(invalid_metrics.transition == EWindowFrameTransition::Invalid);
    Require(invalid_metrics.capture_sequence == 12);
    Require(invalid_metrics.surface_identity == identity_a);
    Require(invalid_metrics.drawable_generation == 5);
    RequireExtent(invalid_metrics.GetStableDrawableExtent(), {1600, 1200});

    // The invalid capture did not replace the last valid identity or metrics.
    const WindowFrameSnapshot stable_after_invalid =
        tracker.Advance(identity_a, MakeMetrics({1024, 768}, {1600, 1200}));
    Require(stable_after_invalid.transition == EWindowFrameTransition::Stable);
    Require(stable_after_invalid.capture_sequence == 13);
    Require(stable_after_invalid.drawable_generation == 5);

    const WindowSurfaceIdentity identity_b = MakeIdentity(0x3000u, 0x4000u, 1u);
    const WindowFrameSnapshot replaced = tracker.Advance(identity_b, MakeMetrics({900, 700}, {1800, 1400}));
    Require(replaced.transition == EWindowFrameTransition::Replaced);
    Require(replaced.capture_sequence == 14);
    Require(replaced.drawable_generation == 6);
    Require(replaced.surface_identity == identity_b);
    RequireExtent(replaced.GetStableDrawableExtent(), {1800, 1400});

    const WindowSurfaceIdentity identity_b_recreated = MakeIdentity(0x3000u, 0x4000u, 2u);
    const WindowFrameSnapshot replaced_zero =
        tracker.Advance(identity_b_recreated, MakeMetrics({900, 700}, {0, 0}));
    Require(replaced_zero.transition == EWindowFrameTransition::Replaced);
    Require(replaced_zero.capture_sequence == 15);
    Require(replaced_zero.drawable_generation == 7);
    Require(!replaced_zero.IsDrawable());
    RequireExtent(replaced_zero.GetStableDrawableExtent(), {0, 0});

    const WindowFrameSnapshot replacement_zero_stable =
        tracker.Advance(identity_b_recreated, MakeMetrics({900, 700}, {0, 0}));
    Require(replacement_zero_stable.transition == EWindowFrameTransition::Stable);
    Require(replacement_zero_stable.drawable_generation == 7);

    const WindowFrameSnapshot replacement_restored =
        tracker.Advance(identity_b_recreated, MakeMetrics({900, 700}, {900, 700}));
    Require(replacement_restored.transition == EWindowFrameTransition::Restored);
    Require(replacement_restored.capture_sequence == 17);
    Require(replacement_restored.drawable_generation == 8);
    RequireExtent(replacement_restored.GetStableDrawableExtent(), {900, 700});

    // Same native handles returning after another identity are still a
    // replacement when the native-window generation changes (ABA defense).
    const WindowSurfaceIdentity identity_a_recreated = MakeIdentity(0x1000u, 0x2000u, 2u);
    const WindowFrameSnapshot aba_replaced =
        tracker.Advance(identity_a_recreated, MakeMetrics({800, 600}, {800, 600}));
    Require(aba_replaced.transition == EWindowFrameTransition::Replaced);
    Require(aba_replaced.capture_sequence == 18);
    Require(aba_replaced.drawable_generation == 9);
    Require(aba_replaced.surface_identity == identity_a_recreated);

    // Mutating a returned value cannot mutate the tracker's internal state.
    WindowFrameSnapshot mutable_copy = aba_replaced;
    mutable_copy.drawable_extent     = {1, 1};
    mutable_copy.drawable_generation = 0;
    const WindowFrameSnapshot stable_after_copy_mutation =
        tracker.Advance(identity_a_recreated, MakeMetrics({800, 600}, {800, 600}));
    Require(stable_after_copy_mutation.transition == EWindowFrameTransition::Stable);
    Require(stable_after_copy_mutation.capture_sequence == 19);
    Require(stable_after_copy_mutation.drawable_generation == 9);
    RequireExtent(stable_after_copy_mutation.drawable_extent, {800, 600});

    WindowFrameSnapshotTracker zero_first_tracker;
    const WindowFrameSnapshot zero_first =
        zero_first_tracker.Advance(identity_a, MakeMetrics({800, 600}, {0, 0}));
    Require(zero_first.transition == EWindowFrameTransition::Minimized);
    Require(zero_first.capture_sequence == 1);
    Require(zero_first.drawable_generation == 0);
    Require(zero_first.IsValid());
    Require(!zero_first.IsDrawable());
    RequireExtent(zero_first.GetStableDrawableExtent(), {0, 0});

    const WindowFrameSnapshot zero_first_repeated =
        zero_first_tracker.Advance(identity_a, MakeMetrics({800, 600}, {0, 0}));
    Require(zero_first_repeated.transition == EWindowFrameTransition::Stable);
    Require(zero_first_repeated.capture_sequence == 2);
    Require(zero_first_repeated.drawable_generation == 0);

    const WindowFrameSnapshot zero_first_restored =
        zero_first_tracker.Advance(identity_a, MakeMetrics({800, 600}, {800, 600}));
    Require(zero_first_restored.transition == EWindowFrameTransition::Restored);
    Require(zero_first_restored.capture_sequence == 3);
    Require(zero_first_restored.drawable_generation == 1);
    RequireExtent(zero_first_restored.GetStableDrawableExtent(), {800, 600});

    WindowFrameSnapshot high_dpi_frame{
        .surface_identity     = identity_a,
        .logical_extent       = {800, 600},
        .drawable_extent      = {1600, 1200},
        .last_drawable_extent = {1600, 1200},
        .capture_sequence     = 1,
        .drawable_generation  = 1,
        .transition           = EWindowFrameTransition::Initial,
    };
    UiDrawFramePacket high_dpi_draw_frame{};
    BindUiViewportWindowFrame(high_dpi_draw_frame.main_viewport, high_dpi_frame);
    Require(high_dpi_draw_frame.main_viewport.framebuffer_scale == Moer::float2(2.f, 2.f));

    UiCompositionFrameData high_dpi_composition{
        .enabled                = true,
        .separate_window        = false,
        .output_resolution      = {800, 600},
        .scene_color_position   = {10.f, 20.f},
        .scene_color_resolution = {320.f, 180.f},
    };
    Require(ResolveUiCompositionDrawableMetrics(
        high_dpi_composition,
        high_dpi_frame,
        high_dpi_draw_frame
    ));
    Require(high_dpi_composition.output_resolution == Moer::uint2(1600, 1200));
    Require(high_dpi_composition.scene_color_position == Moer::float2(20.f, 40.f));
    Require(high_dpi_composition.scene_color_resolution == Moer::float2(640.f, 360.f));

    UiCompositionFrameData unresolved_detached{
        .enabled         = true,
        .separate_window = true,
    };
    Require(!ResolveUiCompositionDrawableMetrics(
        unresolved_detached,
        high_dpi_frame,
        high_dpi_draw_frame
    ));

    return 0;
}
