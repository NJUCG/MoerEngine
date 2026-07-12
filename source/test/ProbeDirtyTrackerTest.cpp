#include "renderer/raster/ProbeDirtyTracker.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using Moer::Box3D;
using Moer::Render::RASTER_PROBE_DIRTY_DYNAMIC;
using Moer::Render::RASTER_PROBE_DIRTY_GEOMETRY;
using Moer::Render::RASTER_PROBE_DIRTY_LIGHT;
using Moer::Render::Raster::ProbeDirtyRegion;
using Moer::Render::Raster::ProbeDirtyTracker;
using Moer::Render::Raster::ProbeTrackedBounds;
using Moer::float3;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "ProbeDirtyTracker test failed: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1e-5f;
}

} // namespace

int main() {
    {
        Moer::Array<ProbeTrackedBounds> previous = {{1u, Box3D(float3(0.0f), float3(1.0f))}};
        Moer::Array<ProbeTrackedBounds> current  = previous;
        const auto diff = ProbeDirtyTracker::Diff(previous, current, RASTER_PROBE_DIRTY_DYNAMIC, 8u);
        Expect(diff.regions.empty(), "unchanged bounds must not produce a dirty region");
    }

    {
        Moer::Array<ProbeTrackedBounds> previous = {{1u, Box3D(float3(0.0f), float3(1.0f))}};
        Moer::Array<ProbeTrackedBounds> current  = {{1u, Box3D(float3(2.0f), float3(3.0f))}};
        const auto diff = ProbeDirtyTracker::Diff(previous, current, RASTER_PROBE_DIRTY_DYNAMIC, 8u);
        Expect(diff.regions.size() == 1u, "moved bounds must produce one union region");
        Expect(NearlyEqual(diff.regions[0].bounds.min.x, 0.0f), "move region must include old bounds");
        Expect(NearlyEqual(diff.regions[0].bounds.max.x, 3.0f), "move region must include new bounds");
    }

    {
        Moer::Array<ProbeTrackedBounds> previous = {
            {1u, Box3D(float3(0.0f), float3(1.0f)), 10u},
        };
        Moer::Array<ProbeTrackedBounds> current = {
            {1u, Box3D(float3(0.0f), float3(1.0f)), 11u},
        };
        const auto diff = ProbeDirtyTracker::Diff(previous, current, RASTER_PROBE_DIRTY_DYNAMIC, 8u);
        Expect(diff.regions.size() == 1u, "state changes must be dirty even when bounds stay equal");
    }

    {
        Moer::Array<ProbeTrackedBounds> previous = {
            {1u, Box3D(float3(0.0f), float3(1.0f))},
            {2u, Box3D(float3(2.0f), float3(3.0f))},
        };
        Moer::Array<ProbeTrackedBounds> current = {
            {2u, Box3D(float3(2.0f), float3(3.0f))},
            {3u, Box3D(float3(4.0f), float3(5.0f))},
        };
        const auto diff = ProbeDirtyTracker::Diff(previous, current, RASTER_PROBE_DIRTY_GEOMETRY, 8u);
        Expect(diff.regions.size() == 2u, "removed and added bounds must both be dirty");
        Expect(diff.changed_bounds == 2u, "removed and added bounds count must be preserved");
    }

    {
        Moer::Array<ProbeTrackedBounds> previous = {
            {1u, Box3D(float3(0.0f), float3(1.0f))},
            {2u, Box3D(float3(2.0f), float3(3.0f))},
        };
        const auto diff = ProbeDirtyTracker::Diff(
            previous,
            previous,
            RASTER_PROBE_DIRTY_GEOMETRY,
            1u,
            true
        );
        Expect(diff.collapsed, "region limit must collapse overflowing dirty bounds");
        Expect(diff.regions.size() == 1u, "collapsed diff must contain one region");
        Expect(NearlyEqual(diff.regions[0].bounds.max.x, 3.0f), "collapsed region must cover all bounds");
    }

    {
        const Box3D probe_bounds(float3(0.0f), float3(1.0f));
        const Box3D nearby_dirty(float3(2.0f, 0.0f, 0.0f), float3(3.0f, 1.0f, 1.0f));
        Expect(!ProbeDirtyTracker::Affects(probe_bounds, nearby_dirty, 0.5f), "short influence must not overlap");
        Expect(ProbeDirtyTracker::Affects(probe_bounds, nearby_dirty, 1.0f), "trace influence must bridge the gap");

        const Moer::Array<ProbeDirtyRegion> regions = {
            {nearby_dirty, RASTER_PROBE_DIRTY_DYNAMIC},
        };
        const uint32_t reasons = ProbeDirtyTracker::ResolveReasons(
            probe_bounds,
            regions,
            RASTER_PROBE_DIRTY_LIGHT,
            1.0f
        );
        Expect((reasons & RASTER_PROBE_DIRTY_DYNAMIC) != 0u, "local dirty reason must be resolved");
        Expect((reasons & RASTER_PROBE_DIRTY_LIGHT) != 0u, "global dirty reason must be preserved");
    }

    std::cout << "ProbeDirtyTracker tests passed." << std::endl;
    return EXIT_SUCCESS;
}
