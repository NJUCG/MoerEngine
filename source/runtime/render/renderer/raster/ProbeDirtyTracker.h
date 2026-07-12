#pragma once

#include "misc/BoundingBox.h"
#include "misc/STL.h"
#include "shaderheaders/shared/raster/ShaderParameters.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace Moer::Render::Raster {

struct ProbeTrackedBounds {
    uint64 key = 0u;
    Box3D  bounds;
    uint64 state_hash = 0u;
};

struct ProbeDirtyRegion {
    Box3D bounds;
    uint  reasons = 0u;
};

struct ProbeDirtyDiff {
    Array<ProbeDirtyRegion> regions;
    uint                    changed_bounds = 0u;
    bool                    collapsed      = false;
};

class ProbeDirtyTracker {
public:
    static void SortByKey(Array<ProbeTrackedBounds>& tracked_bounds) {
        std::sort(
            tracked_bounds.begin(),
            tracked_bounds.end(),
            [](const ProbeTrackedBounds& lhs, const ProbeTrackedBounds& rhs) {
                return lhs.key < rhs.key;
            }
        );
    }

    static ProbeDirtyDiff Diff(
        std::span<const ProbeTrackedBounds> previous,
        std::span<const ProbeTrackedBounds> current,
        uint                                reasons,
        uint                                max_regions,
        bool                                force_existing_dirty = false
    ) {
        ProbeDirtyDiff result;
        if ((reasons & RASTER_PROBE_DIRTY_REASON_MASK) == 0u) {
            return result;
        }

        const uint region_limit = std::max(max_regions, 1u);
        size_t     previous_index = 0u;
        size_t     current_index  = 0u;
        while (previous_index < previous.size() || current_index < current.size()) {
            if (current_index >= current.size() ||
                (previous_index < previous.size() &&
                 previous[previous_index].key < current[current_index].key)) {
                AppendRegion(result, previous[previous_index].bounds, reasons, region_limit);
                ++previous_index;
                continue;
            }

            if (previous_index >= previous.size() ||
                current[current_index].key < previous[previous_index].key) {
                AppendRegion(result, current[current_index].bounds, reasons, region_limit);
                ++current_index;
                continue;
            }

            const ProbeTrackedBounds& old_bounds = previous[previous_index];
            const ProbeTrackedBounds& new_bounds = current[current_index];
            if (force_existing_dirty || old_bounds.state_hash != new_bounds.state_hash ||
                !NearlyEqual(old_bounds.bounds, new_bounds.bounds)) {
                Box3D dirty_bounds = old_bounds.bounds;
                dirty_bounds.Expand(new_bounds.bounds);
                AppendRegion(result, dirty_bounds, reasons, region_limit);
            }
            ++previous_index;
            ++current_index;
        }
        return result;
    }

    static bool Affects(const Box3D& probe_bounds, const Box3D& dirty_bounds, float influence_distance) {
        if (!probe_bounds.IsValid() || !dirty_bounds.IsValid()) {
            return false;
        }

        const float distance = std::max(influence_distance, 0.0f);
        const Box3D expanded_dirty(
            dirty_bounds.min - float3(distance),
            dirty_bounds.max + float3(distance)
        );
        return probe_bounds.min.x <= expanded_dirty.max.x && probe_bounds.max.x >= expanded_dirty.min.x &&
               probe_bounds.min.y <= expanded_dirty.max.y && probe_bounds.max.y >= expanded_dirty.min.y &&
               probe_bounds.min.z <= expanded_dirty.max.z && probe_bounds.max.z >= expanded_dirty.min.z;
    }

    static uint ResolveReasons(
        const Box3D&                    probe_bounds,
        std::span<const ProbeDirtyRegion> regions,
        uint                            global_reasons,
        float                           influence_distance
    ) {
        uint reasons = global_reasons & RASTER_PROBE_DIRTY_REASON_MASK;
        for (const ProbeDirtyRegion& region : regions) {
            if (Affects(probe_bounds, region.bounds, influence_distance)) {
                reasons |= region.reasons & RASTER_PROBE_DIRTY_REASON_MASK;
            }
        }
        return reasons;
    }

private:
    static bool NearlyEqual(float lhs, float rhs) {
        return std::abs(lhs - rhs) <= 1e-5f;
    }

    static bool NearlyEqual(const Box3D& lhs, const Box3D& rhs) {
        return NearlyEqual(lhs.min.x, rhs.min.x) && NearlyEqual(lhs.min.y, rhs.min.y) &&
               NearlyEqual(lhs.min.z, rhs.min.z) && NearlyEqual(lhs.max.x, rhs.max.x) &&
               NearlyEqual(lhs.max.y, rhs.max.y) && NearlyEqual(lhs.max.z, rhs.max.z);
    }

    static void AppendRegion(ProbeDirtyDiff& result, const Box3D& bounds, uint reasons, uint max_regions) {
        if (!bounds.IsValid()) {
            return;
        }

        ++result.changed_bounds;
        if (result.collapsed) {
            result.regions.front().bounds.Expand(bounds);
            result.regions.front().reasons |= reasons;
            return;
        }

        if (result.regions.size() < max_regions) {
            result.regions.push_back({bounds, reasons});
            return;
        }

        Box3D collapsed_bounds = bounds;
        uint  collapsed_reasons = reasons;
        for (const ProbeDirtyRegion& region : result.regions) {
            collapsed_bounds.Expand(region.bounds);
            collapsed_reasons |= region.reasons;
        }
        result.regions.clear();
        result.regions.push_back({collapsed_bounds, collapsed_reasons});
        result.collapsed = true;
    }
};

} // namespace Moer::Render::Raster
