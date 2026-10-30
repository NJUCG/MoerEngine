#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"
#include "misc/Types.h"

#include <algorithm>

namespace Moer::Render::Raster {

class ProbePhysicalAllocator {
public:
    struct Allocation {
        uint32 probe_offset = 0;
        uint32 probe_count  = 0;
        bool valid        = false;
    };

    void Reset(uint32 capacity) {
        m_capacity = capacity;
        m_free_ranges.clear();
        if (capacity > 0u) {
            m_free_ranges.push_back(Range{0u, capacity});
        }
    }

    bool Allocate(uint32 probe_count, Allocation& allocation) {
        allocation = {};
        if (probe_count == 0u) {
            return false;
        }

        for (size_t range_index = 0; range_index < m_free_ranges.size(); ++range_index) {
            Range& range = m_free_ranges[range_index];
            if (range.probe_count < probe_count) {
                continue;
            }

            allocation = Allocation{range.probe_offset, probe_count, true};
            range.probe_offset += probe_count;
            range.probe_count -= probe_count;
            if (range.probe_count == 0u) {
                m_free_ranges.erase(m_free_ranges.begin() + range_index);
            }
            return true;
        }
        return false;
    }

    bool Release(const Allocation& allocation) {
        if (!allocation.valid || allocation.probe_count == 0u ||
            allocation.probe_offset > m_capacity ||
            allocation.probe_count > m_capacity - allocation.probe_offset) {
            return false;
        }

        const uint32 release_end = allocation.probe_offset + allocation.probe_count;
        for (const Range& range : m_free_ranges) {
            const uint32 range_end = range.probe_offset + range.probe_count;
            if (allocation.probe_offset < range_end && range.probe_offset < release_end) {
                return false;
            }
        }

        m_free_ranges.push_back(Range{allocation.probe_offset, allocation.probe_count});
        CoalesceFreeRanges();
        return true;
    }

    uint32 GetCapacity() const {
        return m_capacity;
    }

    uint32 GetFreeProbeCount() const {
        uint32 free_probe_count = 0;
        for (const Range& range : m_free_ranges) {
            free_probe_count += range.probe_count;
        }
        return free_probe_count;
    }

    uint32 GetLargestFreeRange() const {
        uint32 largest_range = 0;
        for (const Range& range : m_free_ranges) {
            largest_range = std::max(largest_range, range.probe_count);
        }
        return largest_range;
    }

    bool Validate() const {
        uint32 previous_end = 0;
        uint32 free_count   = 0;
        for (size_t range_index = 0; range_index < m_free_ranges.size(); ++range_index) {
            const Range& range = m_free_ranges[range_index];
            if (range.probe_count == 0u || range.probe_offset > m_capacity ||
                range.probe_count > m_capacity - range.probe_offset) {
                return false;
            }
            if (range_index > 0 && range.probe_offset <= previous_end) {
                return false;
            }
            previous_end = range.probe_offset + range.probe_count;
            free_count += range.probe_count;
        }
        return free_count <= m_capacity;
    }

private:
    struct Range {
        uint32 probe_offset = 0;
        uint32 probe_count  = 0;
    };

    void CoalesceFreeRanges() {
        if (m_free_ranges.size() < 2) {
            return;
        }

        std::sort(m_free_ranges.begin(), m_free_ranges.end(), [](const Range& lhs, const Range& rhs) {
            return lhs.probe_offset < rhs.probe_offset;
        });

        Array<Range> merged_ranges;
        merged_ranges.reserve(m_free_ranges.size());
        for (const Range& range : m_free_ranges) {
            if (merged_ranges.empty()) {
                merged_ranges.push_back(range);
                continue;
            }

            Range& previous = merged_ranges.back();
            const uint32 previous_end = previous.probe_offset + previous.probe_count;
            if (range.probe_offset <= previous_end) {
                const uint32 merged_end = std::max(previous_end, range.probe_offset + range.probe_count);
                previous.probe_count = merged_end - previous.probe_offset;
                continue;
            }
            merged_ranges.push_back(range);
        }
        m_free_ranges = std::move(merged_ranges);
    }

    uint32       m_capacity = 0;
    Array<Range> m_free_ranges;
};

} // namespace Moer::Render::Raster
