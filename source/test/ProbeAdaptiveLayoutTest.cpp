#include "renderer/raster/ProbeAdaptiveLayout.h"

#include <cmath>
#include <iostream>

namespace {

using Moer::Render::Raster::ProbeAdaptiveLayout;
using Moer::Render::Raster::ProbeBrickVirtualKey;
using namespace Moer::Render;
using Moer::uint;
using Moer::uint3;

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAILED] " << message << std::endl;
    }
    return condition;
}

bool Equal(uint3 lhs, uint3 rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1e-6f;
}

bool TestStableLevelRanges() {
    return Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 0u, uint3(0u, 0u, 0u)}) == 0u,
               "level 0 must begin at page 0"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 0u, uint3(3u, 3u, 3u)}) == 63u,
               "level 0 must occupy pages 0 through 63"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 0u, uint3(0u, 1u, 0u)}) == 4u,
               "level 0 Y coordinates must use the fixed four-page row stride"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 0u, uint3(0u, 0u, 1u)}) == 16u,
               "level 0 Z coordinates must use the fixed sixteen-page slice stride"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 1u, uint3(0u, 0u, 0u)}) == 64u,
               "level 1 must begin at page 64"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 1u, uint3(1u, 1u, 1u)}) == 71u,
               "level 1 must occupy pages 64 through 71"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 2u, uint3(0u, 0u, 0u)}) == 72u,
               "level 2 root must occupy page 72"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({1u, 0u, uint3(0u, 0u, 0u)}) ==
                   RASTER_PROBE_MAX_PAGES_PER_VOLUME,
               "volume slots must have disjoint stable page ranges"
           );
}

bool TestParentAndNeighbors() {
    const ProbeBrickVirtualKey key{2u, 0u, uint3(3u, 2u, 1u)};
    const ProbeBrickVirtualKey parent = ProbeAdaptiveLayout::GetParent(key);
    const uint expected_parent_page = 2u * RASTER_PROBE_MAX_PAGES_PER_VOLUME + 64u + 3u;

    return Expect(parent == ProbeBrickVirtualKey{2u, 1u, uint3(1u, 1u, 0u)}, "parent key must halve coordinates") &&
           Expect(
               ProbeAdaptiveLayout::GetParentPageIndex(key) == expected_parent_page,
               "parent page must use the stable level 1 range"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetNeighborPageIndex(key, -1, 0, 0, uint3(4u, 4u, 4u)) ==
                   ProbeAdaptiveLayout::GetVirtualPageIndex({2u, 0u, uint3(2u, 2u, 1u)}),
               "valid neighbor must resolve to its virtual page"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetNeighborPageIndex(key, 1, 0, 0, uint3(4u, 4u, 4u)) ==
                   RASTER_PROBE_PAGE_INVALID,
               "out-of-bounds neighbor must be invalid"
           );
}

bool TestCellPartition() {
    return Expect(
               Equal(ProbeAdaptiveLayout::GetCellCounts(uint3(4u, 3u, 1u)), uint3(2u, 2u, 1u)),
               "cell counts must ceil-divide level 0 bricks"
           ) &&
           Expect(
               Equal(ProbeAdaptiveLayout::GetCellCoord(uint3(3u, 2u, 0u)), uint3(1u, 1u, 0u)),
               "cell coordinates must group 2x2x2 fine bricks"
           );
}

bool TestLevelProbeLayout() {
    const uint3 base_counts(16u, 9u, 1u);
    const uint3 level_0_counts = ProbeAdaptiveLayout::GetLevelProbeCounts(base_counts, 0u);
    const uint3 level_1_counts = ProbeAdaptiveLayout::GetLevelProbeCounts(base_counts, 1u);
    const uint3 level_2_counts = ProbeAdaptiveLayout::GetLevelProbeCounts(base_counts, 2u);
    const uint3 level_1_bricks = ProbeAdaptiveLayout::GetLevelBrickCounts(base_counts, 1u);
    const uint3 level_2_bricks = ProbeAdaptiveLayout::GetLevelBrickCounts(base_counts, 2u);
    const Moer::float3 level_1_spacing =
        ProbeAdaptiveLayout::GetLevelSpacing(Moer::float3(15.0f, 8.0f, 6.0f), level_1_counts);

    return Expect(Equal(level_0_counts, base_counts), "level 0 counts must preserve the base grid") &&
           Expect(
               Equal(level_1_counts, uint3(8u, 5u, 1u)),
               "level 1 counts must ceil-divide the base grid by two"
           ) &&
           Expect(
               Equal(level_2_counts, uint3(4u, 3u, 1u)),
               "level 2 counts must ceil-divide the base grid by four"
           ) &&
           Expect(
               Equal(level_1_bricks, uint3(2u, 2u, 1u)),
               "level 1 probes must fit the fixed 2x2x2 virtual page grid"
           ) &&
           Expect(
               Equal(level_2_bricks, uint3(1u, 1u, 1u)),
               "level 2 probes must fit the root virtual page"
           ) &&
           Expect(
               NearlyEqual(level_1_spacing.x * float(level_1_counts.x - 1u), 15.0f) &&
                   NearlyEqual(level_1_spacing.y * float(level_1_counts.y - 1u), 8.0f) &&
                   NearlyEqual(level_1_spacing.z, 6.0f),
               "coarse spacing must preserve both volume endpoints and singleton axes"
           );
}

bool TestAdaptiveLevelRequests() {
    return Expect(
               ProbeAdaptiveLayout::ShouldRequestLevel(0u, 0u, true) &&
                   ProbeAdaptiveLayout::ShouldRequestLevel(0u, 1u, true) &&
                   ProbeAdaptiveLayout::ShouldRequestLevel(0u, 2u, true),
               "fine cells must retain both parent fallback levels"
           ) &&
           Expect(
               !ProbeAdaptiveLayout::ShouldRequestLevel(1u, 0u, true) &&
                   ProbeAdaptiveLayout::ShouldRequestLevel(1u, 1u, true) &&
                   ProbeAdaptiveLayout::ShouldRequestLevel(1u, 2u, true),
               "medium cells must request level 1 and the root"
           ) &&
           Expect(
               !ProbeAdaptiveLayout::ShouldRequestLevel(2u, 0u, true) &&
                   !ProbeAdaptiveLayout::ShouldRequestLevel(2u, 1u, true) &&
                   ProbeAdaptiveLayout::ShouldRequestLevel(2u, 2u, true),
               "coarse cells must request only the root"
           ) &&
           Expect(
               ProbeAdaptiveLayout::ShouldRequestLevel(2u, 0u, false) &&
                   !ProbeAdaptiveLayout::ShouldRequestLevel(0u, 1u, false),
               "disabled hierarchy must preserve legacy level 0 residency"
           );
}

bool TestInvalidKeys() {
    return Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex(
                   {RASTER_PROBE_VOLUME_MAX_COUNT, 0u, uint3(0u, 0u, 0u)}
               ) == RASTER_PROBE_PAGE_INVALID,
               "invalid volume slot must not encode"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetVirtualPageIndex({0u, 1u, uint3(2u, 0u, 0u)}) ==
                   RASTER_PROBE_PAGE_INVALID,
               "coordinates outside the level grid must not encode"
           ) &&
           Expect(
               ProbeAdaptiveLayout::GetParentPageIndex({0u, 2u, uint3(0u, 0u, 0u)}) ==
                   RASTER_PROBE_PAGE_INVALID,
               "root level must not have a parent"
           );
}

} // namespace

int main() {
    const bool passed =
        TestStableLevelRanges() && TestParentAndNeighbors() && TestCellPartition() &&
        TestLevelProbeLayout() && TestAdaptiveLevelRequests() && TestInvalidKeys();
    if (!passed) {
        return 1;
    }
    std::cout << "ProbeAdaptiveLayout tests passed." << std::endl;
    return 0;
}
