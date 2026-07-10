#include "renderer/raster/ProbePhysicalAllocator.h"

#include <iostream>

namespace {

using Moer::Render::Raster::ProbePhysicalAllocator;

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAILED] " << message << std::endl;
    }
    return condition;
}

bool TestReleasedRangeIsReused() {
    ProbePhysicalAllocator allocator;
    ProbePhysicalAllocator::Allocation first;
    ProbePhysicalAllocator::Allocation second;
    ProbePhysicalAllocator::Allocation reused;

    allocator.Reset(128u);
    return Expect(allocator.Allocate(64u, first) && first.probe_offset == 0u, "first range must start at 0") &&
           Expect(allocator.Allocate(64u, second) && second.probe_offset == 64u, "second range must start at 64") &&
           Expect(allocator.GetFreeProbeCount() == 0u, "allocator must be full") &&
           Expect(allocator.Release(first), "first range release must succeed") &&
           Expect(allocator.Allocate(64u, reused), "released range must be allocatable") &&
           Expect(reused.probe_offset == first.probe_offset, "released offset must be reused") &&
           Expect(allocator.Validate(), "allocator invariants must hold after reuse");
}

bool TestAdjacentRangesCoalesce() {
    ProbePhysicalAllocator allocator;
    ProbePhysicalAllocator::Allocation first;
    ProbePhysicalAllocator::Allocation second;
    ProbePhysicalAllocator::Allocation third;
    ProbePhysicalAllocator::Allocation merged;

    allocator.Reset(192u);
    if (!allocator.Allocate(64u, first) || !allocator.Allocate(64u, second) ||
        !allocator.Allocate(64u, third)) {
        return Expect(false, "setup allocations must succeed");
    }

    return Expect(allocator.Release(second), "middle range release must succeed") &&
           Expect(allocator.Release(first), "first range release must succeed") &&
           Expect(allocator.GetLargestFreeRange() == 128u, "adjacent ranges must coalesce") &&
           Expect(allocator.Allocate(96u, merged) && merged.probe_offset == 0u, "merged range must serve allocation") &&
           Expect(allocator.Release(merged), "merged allocation release must succeed") &&
           Expect(allocator.Release(third), "third range release must succeed") &&
           Expect(allocator.GetFreeProbeCount() == 192u, "all capacity must be free") &&
           Expect(allocator.GetLargestFreeRange() == 192u, "all free ranges must coalesce") &&
           Expect(allocator.Validate(), "allocator invariants must hold after coalescing");
}

bool TestInvalidOperationsDoNotCorruptState() {
    ProbePhysicalAllocator allocator;
    ProbePhysicalAllocator::Allocation allocation;
    ProbePhysicalAllocator::Allocation failed;

    allocator.Reset(64u);
    if (!allocator.Allocate(48u, allocation)) {
        return Expect(false, "setup allocation must succeed");
    }

    return Expect(!allocator.Allocate(32u, failed), "oversized allocation must fail") &&
           Expect(!failed.valid, "failed allocation must be invalid") &&
           Expect(allocator.GetFreeProbeCount() == 16u, "failed allocation must not consume capacity") &&
           Expect(allocator.Release(allocation), "valid release must succeed") &&
           Expect(!allocator.Release(allocation), "double release must be rejected") &&
           Expect(allocator.GetFreeProbeCount() == 64u, "double release must not increase free capacity") &&
           Expect(allocator.Validate(), "allocator invariants must hold after rejected operations");
}

} // namespace

int main() {
    const bool passed = TestReleasedRangeIsReused() && TestAdjacentRangesCoalesce() &&
                        TestInvalidOperationsDoNotCorruptState();
    if (!passed) {
        return 1;
    }
    std::cout << "ProbePhysicalAllocator tests passed." << std::endl;
    return 0;
}
