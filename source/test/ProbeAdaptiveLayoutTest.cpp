#include "renderer/raster/ProbeAdaptiveLayout.h"

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
        TestStableLevelRanges() && TestParentAndNeighbors() && TestCellPartition() && TestInvalidKeys();
    if (!passed) {
        return 1;
    }
    std::cout << "ProbeAdaptiveLayout tests passed." << std::endl;
    return 0;
}
