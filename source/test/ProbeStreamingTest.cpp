#include "renderer/raster/ProbeStreaming.h"

#include <iostream>

namespace {

using namespace Moer;
using namespace Moer::Render;
using namespace Moer::Render::Raster;

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAILED] " << message << std::endl;
    }
    return condition;
}

bool TestPageEntryRoundTrip() {
    const uint entry = PackProbePageEntry(511u, 0xabcdeu, RASTER_PROBE_STREAMING_RESIDENT);
    return Expect(entry != RASTER_PROBE_PAGE_INVALID, "valid page entry must not alias invalid") &&
           Expect(UnpackProbePageBrickIndex(entry) == 511u, "brick index must round-trip") &&
           Expect(UnpackProbePageGeneration(entry) == 0xabcdeu, "generation must round-trip") &&
           Expect(
               UnpackProbePageStreamingState(entry) == RASTER_PROBE_STREAMING_RESIDENT,
               "streaming state must round-trip"
           ) &&
           Expect(NextProbePageGeneration(RASTER_PROBE_PAGE_GENERATION_MASK) == 1u, "generation wrap must skip zero");
}

bool TestStreamingFlagsPreserveDirtyState() {
    const uint dirty_flags = RASTER_PROBE_DIRTY_GEOMETRY | RASTER_PROBE_DIRTY_SCHEDULED;
    const uint pending_flags = PackProbeStreamingFlags(
        dirty_flags | RASTER_PROBE_STREAMING_STATE_FLAG_MASK | RASTER_PROBE_STREAMING_CACHED,
        RASTER_PROBE_STREAMING_PENDING_LOAD,
        false
    );
    const uint resident_flags = PackProbeStreamingFlags(
        dirty_flags,
        RASTER_PROBE_STREAMING_RESIDENT,
        true
    );

    return Expect(
               (pending_flags & dirty_flags) == dirty_flags,
               "streaming flags must preserve dirty reasons"
           ) &&
           Expect(
               (pending_flags & RASTER_PROBE_STREAMING_STATE_FLAG_MASK) ==
                   (RASTER_PROBE_STREAMING_PENDING_LOAD << RASTER_PROBE_STREAMING_STATE_FLAG_SHIFT),
               "pending state must replace previous streaming bits"
           ) &&
           Expect(
               (pending_flags & RASTER_PROBE_STREAMING_CACHED) == 0u,
               "pending non-cached page must clear cached bit"
           ) &&
           Expect(
               (resident_flags & RASTER_PROBE_STREAMING_CACHED) != 0u,
               "resident cached page must set cached bit"
           );
}

bool TestRetirementWaitsForCompletedSubmission() {
    ProbePhysicalAllocator allocator;
    ProbePhysicalAllocator::Allocation allocation;
    ProbeRetirementQueue retirements;
    allocator.Reset(128u);
    if (!allocator.Allocate(64u, allocation)) {
        return Expect(false, "setup allocation must succeed");
    }

    const ProbeRetirementQueue::Entry entry{
        allocation,
        3u,
        17u,
        9u,
        42u,
    };
    if (!retirements.Enqueue(entry)) {
        return Expect(false, "valid allocation must enter retirement queue");
    }

    const auto early = retirements.Collect(41u, allocator);
    if (!Expect(early.allocation_count == 0u, "in-flight allocation must not be reclaimed") ||
        !Expect(allocator.GetFreeProbeCount() == 64u, "retiring allocation must remain unavailable") ||
        !Expect(retirements.ContainsVirtualPage(17u), "retiring page must remain discoverable")) {
        return false;
    }

    const auto completed = retirements.Collect(42u, allocator);
    return Expect(completed.allocation_count == 1u, "completed allocation must be reclaimed") &&
           Expect(completed.probe_count == 64u, "reclaimed probe count must be reported") &&
           Expect(completed.failed_count == 0u, "valid reclaim must not fail") &&
           Expect(retirements.Empty(), "completed retirement queue must be empty") &&
           Expect(allocator.GetFreeProbeCount() == 128u, "reclaimed range must return to allocator") &&
           Expect(allocator.Validate(), "allocator invariants must survive retirement");
}

} // namespace

int main() {
    if (!TestPageEntryRoundTrip() || !TestStreamingFlagsPreserveDirtyState() ||
        !TestRetirementWaitsForCompletedSubmission()) {
        return 1;
    }
    std::cout << "Probe streaming tests passed." << std::endl;
    return 0;
}
