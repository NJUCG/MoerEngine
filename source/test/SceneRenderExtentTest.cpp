#include "renderer/SceneRenderExtent.h"

#include <iostream>
#include <limits>
#include <string_view>

namespace {

class TestSuite {
public:
    void Check(bool condition, std::string_view message) {
        if (condition) {
            return;
        }
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }

    [[nodiscard]] int FailureCount() const {
        return failures;
    }

private:
    int failures = 0;
};

} // namespace

int main() {
    using namespace Moer;
    using namespace Moer::Render;

    TestSuite suite;

    const auto headless = CaptureSceneRenderExtentRequest(
        false, float2(0.f, 0.f), uint2(1600u, 900u)
    );
    suite.Check(headless.valid, "UI-disabled window extent must be valid");
    suite.Check(headless.immediate, "UI-disabled window extent must bypass debounce");
    suite.Check(
        EqualRenderExtent(headless.extent, uint2(1600u, 900u)),
        "UI-disabled scene extent must follow the window"
    );

    const auto docked = CaptureSceneRenderExtentRequest(
        true, float2(913.9f, 513.2f), uint2(1600u, 900u)
    );
    suite.Check(docked.valid && !docked.immediate, "docked extent must be debounced");
    suite.Check(
        EqualRenderExtent(docked.extent, uint2(913u, 513u)),
        "docked floating-point extent must be frozen to integer pixels"
    );

    const auto collapsed = CaptureSceneRenderExtentRequest(
        true, float2(0.f, 0.f), uint2(1600u, 900u)
    );
    suite.Check(!collapsed.valid, "collapsed SceneColor must not request zero-sized resources");

    const auto non_finite = CaptureSceneRenderExtentRequest(
        true,
        float2(std::numeric_limits<float>::infinity(), 900.f),
        uint2(1600u, 900u)
    );
    suite.Check(!non_finite.valid, "non-finite SceneColor extent must be rejected");

    const auto tiny = CaptureSceneRenderExtentRequest(
        true, float2(4.f, 7.f), uint2(1600u, 900u)
    );
    suite.Check(
        EqualRenderExtent(
            tiny.extent,
            uint2(k_min_scene_render_extent, k_min_scene_render_extent)
        ),
        "scene extent must preserve the fixed six-mip bloom contract"
    );

    SceneRenderExtentTracker tracker(uint2(1600u, 900u));
    suite.Check(!tracker.Observe(docked), "first docked observation must remain pending");
    suite.Check(
        EqualRenderExtent(tracker.GetActiveExtent(), uint2(1600u, 900u)),
        "pending request must retain the allocated extent"
    );
    suite.Check(tracker.Observe(docked), "second stable observation must commit");
    suite.Check(
        EqualRenderExtent(tracker.GetActiveExtent(), docked.extent),
        "committed docked request must become active"
    );

    const auto first_drag = CaptureSceneRenderExtentRequest(
        true, float2(917.f, 517.f), uint2(1600u, 900u)
    );
    const auto second_drag = CaptureSceneRenderExtentRequest(
        true, float2(920.f, 520.f), uint2(1600u, 900u)
    );
    suite.Check(!tracker.Observe(first_drag), "drag request must start pending");
    suite.Check(!tracker.Observe(second_drag), "changed drag request must restart debounce");
    suite.Check(
        !tracker.Observe(collapsed),
        "invalid request must retain the active allocation"
    );
    suite.Check(!tracker.HasPendingExtent(), "invalid request must clear stale pending state");
    suite.Check(
        EqualRenderExtent(tracker.GetActiveExtent(), docked.extent),
        "collapsed SceneColor must retain the last valid allocation"
    );

    const auto window_resize = CaptureSceneRenderExtentRequest(
        false, float2(0.f, 0.f), uint2(1920u, 1080u)
    );
    suite.Check(tracker.Observe(window_resize), "headless window resize must commit immediately");
    suite.Check(
        EqualRenderExtent(tracker.GetActiveExtent(), uint2(1920u, 1080u)),
        "immediate request must update the active extent"
    );

    if (suite.FailureCount() != 0) {
        std::cerr << "TestSceneRenderExtent: " << suite.FailureCount() << " failure(s)\n";
        return 1;
    }
    std::cout << "TestSceneRenderExtent: all checks passed\n";
    return 0;
}
