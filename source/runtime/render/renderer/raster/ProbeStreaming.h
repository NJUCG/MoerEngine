#pragma once

#include "ProbePhysicalAllocator.h"
#include "misc/Types.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include <algorithm>

namespace Moer::Render::Raster {

inline uint32 NormalizeProbePageGeneration(uint32 generation) {
    generation &= RASTER_PROBE_PAGE_GENERATION_MASK;
    return generation == 0u ? 1u : generation;
}

inline uint32 NextProbePageGeneration(uint32 generation) {
    return NormalizeProbePageGeneration(generation + 1u);
}

inline uint32 PackProbePageEntry(uint32 brick_index, uint32 generation, uint32 streaming_state) {
    return (brick_index & RASTER_PROBE_PAGE_BRICK_MASK) |
           (NormalizeProbePageGeneration(generation) << RASTER_PROBE_PAGE_GENERATION_SHIFT) |
           ((streaming_state & RASTER_PROBE_PAGE_STATE_MASK) << RASTER_PROBE_PAGE_STATE_SHIFT);
}

inline uint32 UnpackProbePageBrickIndex(uint32 entry) {
    return entry & RASTER_PROBE_PAGE_BRICK_MASK;
}

inline uint32 UnpackProbePageGeneration(uint32 entry) {
    return (entry >> RASTER_PROBE_PAGE_GENERATION_SHIFT) & RASTER_PROBE_PAGE_GENERATION_MASK;
}

inline uint32 UnpackProbePageStreamingState(uint32 entry) {
    return (entry >> RASTER_PROBE_PAGE_STATE_SHIFT) & RASTER_PROBE_PAGE_STATE_MASK;
}

inline uint32 PackProbeStreamingFlags(
    uint32 flags,
    uint32 streaming_state,
    bool   cached,
    bool   prefetched     = false,
    bool   clipmap_reused = false
) {
    flags &= ~(RASTER_PROBE_STREAMING_STATE_FLAG_MASK | RASTER_PROBE_STREAMING_CACHED |
               RASTER_PROBE_STREAMING_PREFETCHED | RASTER_PROBE_CLIPMAP_REUSED);
    flags |= (streaming_state & RASTER_PROBE_PAGE_STATE_MASK)
             << RASTER_PROBE_STREAMING_STATE_FLAG_SHIFT;
    if (cached) {
        flags |= RASTER_PROBE_STREAMING_CACHED;
    }
    if (prefetched) {
        flags |= RASTER_PROBE_STREAMING_PREFETCHED;
    }
    if (clipmap_reused) {
        flags |= RASTER_PROBE_CLIPMAP_REUSED;
    }
    return flags;
}

class ProbeRetirementQueue {
public:
    struct Entry {
        ProbePhysicalAllocator::Allocation allocation{};
        uint32                             brick_index           = RASTER_PROBE_PAGE_INVALID;
        uint32                             virtual_page          = RASTER_PROBE_PAGE_INVALID;
        uint32                             page_generation       = 0u;
        uint64                             safe_after_submission = 0u;
    };

    struct CollectResult {
        uint32 allocation_count = 0u;
        uint32 probe_count      = 0u;
        uint32 failed_count     = 0u;
    };

    bool Enqueue(const Entry& entry) {
        if (!entry.allocation.valid || entry.allocation.probe_count == 0u) {
            return false;
        }
        m_entries.push_back(entry);
        return true;
    }

    CollectResult Collect(uint64 completed_submission, ProbePhysicalAllocator& allocator) {
        CollectResult result{};
        auto iterator = m_entries.begin();
        while (iterator != m_entries.end()) {
            if (iterator->safe_after_submission > completed_submission) {
                ++iterator;
                continue;
            }

            if (allocator.Release(iterator->allocation)) {
                ++result.allocation_count;
                result.probe_count += iterator->allocation.probe_count;
            } else {
                ++result.failed_count;
            }
            iterator = m_entries.erase(iterator);
        }
        return result;
    }

    bool ContainsVirtualPage(uint32 virtual_page) const {
        return std::any_of(m_entries.begin(), m_entries.end(), [virtual_page](const Entry& entry) {
            return entry.virtual_page == virtual_page;
        });
    }

    uint32 GetAllocationCount() const {
        return static_cast<uint32>(m_entries.size());
    }

    uint32 GetProbeCount() const {
        uint32 probe_count = 0u;
        for (const Entry& entry : m_entries) {
            probe_count += entry.allocation.probe_count;
        }
        return probe_count;
    }

    bool Empty() const {
        return m_entries.empty();
    }

    void Clear() {
        m_entries.clear();
    }

private:
    Array<Entry> m_entries;
};

static_assert(RASTER_PROBE_MAX_BRICK_COUNT <= RASTER_PROBE_PAGE_BRICK_MASK + 1u);
static_assert(
    RASTER_PROBE_PAGE_BRICK_BITS + RASTER_PROBE_PAGE_GENERATION_BITS + 2u == 32u
);

} // namespace Moer::Render::Raster
