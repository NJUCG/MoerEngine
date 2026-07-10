#pragma once

#include "misc/STL.h"
#include "misc/Traits.h"

#include <algorithm>

namespace Moer::Render::Raster {

class ProbePhysicalAllocator {
public:
    struct Allocation {
        uint probe_offset = 0;
        uint probe_count  = 0;
        bool valid        = false;
    };

    void Reset(uint capacity) {
        m_capacity = capacity;
        m_free_ranges.clear();
        if (capacity > 0u) {
            m_free_ranges.push_back(Range{0u, capacity});
        }
    }

    bool Allocate(uint probe_count, Allocation& allocation) {
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

        const uint release_end = allocation.probe_offset + allocation.probe_count;
        for (const Range& range : m_free_ranges) {
            const uint range_end = range.probe_offset + range.probe_count;
            if (allocation.probe_offset < range_end && range.probe_offset < release_end) {
                return false;
            }
        }

        m_free_ranges.push_back(Range{allocation.probe_offset, allocation.probe_count});
        CoalesceFreeRanges();
        return true;
    }

    uint GetCapacity() const {
        return m_capacity;
    }

    uint GetFreeProbeCount() const {
        uint free_probe_count = 0;
        for (const Range& range : m_free_ranges) {
            free_probe_count += range.probe_count;
        }
        return free_probe_count;
    }

    uint GetLargestFreeRange() const {
        uint largest_range = 0;
        for (const Range& range : m_free_ranges) {
            largest_range = std::max(largest_range, range.probe_count);
        }
        return largest_range;
    }

    bool Validate() const {
        uint previous_end = 0;
        uint free_count   = 0;
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
        uint probe_offset = 0;
        uint probe_count  = 0;
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
            const uint previous_end = previous.probe_offset + previous.probe_count;
            if (range.probe_offset <= previous_end) {
                const uint merged_end = std::max(previous_end, range.probe_offset + range.probe_count);
                previous.probe_count = merged_end - previous.probe_offset;
                continue;
            }
            merged_ranges.push_back(range);
        }
        m_free_ranges = std::move(merged_ranges);
    }

    uint         m_capacity = 0;
    Array<Range> m_free_ranges;
};

} // namespace Moer::Render::Raster
