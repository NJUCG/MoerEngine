#pragma once

#include "ProbeAdaptiveLayout.h"
#include "misc/BoundingBox.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>

namespace Moer::Render::Raster {

struct ProbeGeometryCellStats {
    uint  intersecting_primitive_count = 0u;
    uint  occupied_voxel_count         = 0u;
    uint  desired_subdivision_level    = RASTER_PROBE_MAX_SUBDIVISION_LEVEL;
    float occupancy                    = 0.0f;
};

class ProbeGeometryClassifier {
public:
    static constexpr uint OCCUPANCY_GRID_DIM    = RASTER_PROBE_OCCUPANCY_GRID_DIM;
    static constexpr uint OCCUPANCY_VOXEL_COUNT = RASTER_PROBE_OCCUPANCY_VOXEL_COUNT;

    static Box3D BuildCellInfluenceBounds(
        float3 cell_origin,
        float3 cell_extent,
        float3 probe_spacing,
        const Box3D& volume_bounds
    ) {
        const float3 half_spacing = probe_spacing * 0.5f;
        Box3D        bounds(
            Max(cell_origin - half_spacing, volume_bounds.min),
            Min(cell_origin + cell_extent + half_spacing, volume_bounds.max)
        );

        constexpr float zero_extent = 1e-6f;
        const float3 volume_extent = volume_bounds.GetExtent();
        if (cell_extent.x <= zero_extent && volume_extent.x <= probe_spacing.x + zero_extent) {
            bounds.min.x = volume_bounds.min.x;
            bounds.max.x = volume_bounds.max.x;
        }
        if (cell_extent.y <= zero_extent && volume_extent.y <= probe_spacing.y + zero_extent) {
            bounds.min.y = volume_bounds.min.y;
            bounds.max.y = volume_bounds.max.y;
        }
        if (cell_extent.z <= zero_extent && volume_extent.z <= probe_spacing.z + zero_extent) {
            bounds.min.z = volume_bounds.min.z;
            bounds.max.z = volume_bounds.max.z;
        }
        return bounds;
    }

    static ProbeGeometryCellStats Analyze(
        const Box3D&             cell_bounds,
        std::span<const Box3D>   primitive_bounds,
        float                    geometry_padding,
        float                    fine_occupancy_threshold,
        uint                     fine_primitive_threshold
    ) {
        ProbeGeometryCellStats stats{};
        if (!cell_bounds.IsValid()) {
            return stats;
        }

        const float3 cell_extent = cell_bounds.GetExtent();
        if (cell_extent.x <= 1e-6f || cell_extent.y <= 1e-6f || cell_extent.z <= 1e-6f) {
            return stats;
        }

        const float padding = std::max(geometry_padding, 0.0f);
        uint64_t    occupancy_mask = 0u;
        for (const Box3D& primitive_bound : primitive_bounds) {
            if (!primitive_bound.IsValid()) {
                continue;
            }

            const Box3D padded_bound(
                primitive_bound.min - float3(padding),
                primitive_bound.max + float3(padding)
            );
            const float3 overlap_min = Max(cell_bounds.min, padded_bound.min);
            const float3 overlap_max = Min(cell_bounds.max, padded_bound.max);
            if (overlap_min.x > overlap_max.x || overlap_min.y > overlap_max.y ||
                overlap_min.z > overlap_max.z) {
                continue;
            }

            ++stats.intersecting_primitive_count;
            const uint3 voxel_min = ToVoxelCoord(overlap_min, cell_bounds.min, cell_extent);
            const uint3 voxel_max = ToVoxelCoord(overlap_max, cell_bounds.min, cell_extent);
            for (uint z = voxel_min.z; z <= voxel_max.z; ++z) {
                for (uint y = voxel_min.y; y <= voxel_max.y; ++y) {
                    for (uint x = voxel_min.x; x <= voxel_max.x; ++x) {
                        const uint bit_index = x + y * OCCUPANCY_GRID_DIM +
                                               z * OCCUPANCY_GRID_DIM * OCCUPANCY_GRID_DIM;
                        occupancy_mask |= uint64_t(1) << bit_index;
                    }
                }
            }
        }

        stats.occupied_voxel_count = static_cast<uint>(std::popcount(occupancy_mask));
        stats.occupancy = float(stats.occupied_voxel_count) / float(OCCUPANCY_VOXEL_COUNT);
        if (stats.intersecting_primitive_count == 0u) {
            stats.desired_subdivision_level = RASTER_PROBE_MAX_SUBDIVISION_LEVEL;
            return stats;
        }

        const float threshold = std::clamp(
            fine_occupancy_threshold,
            1.0f / float(OCCUPANCY_VOXEL_COUNT),
            1.0f
        );
        const bool requires_fine_level =
            stats.occupancy >= threshold ||
            stats.intersecting_primitive_count >= std::max(fine_primitive_threshold, 1u);
        stats.desired_subdivision_level =
            requires_fine_level ? 0u : Min(1u, RASTER_PROBE_MAX_SUBDIVISION_LEVEL);
        return stats;
    }

private:
    static uint3 ToVoxelCoord(float3 point, float3 cell_min, float3 cell_extent) {
        const float3 normalized = (point - cell_min) / cell_extent;
        return uint3(
            ToVoxelCoord(normalized.x),
            ToVoxelCoord(normalized.y),
            ToVoxelCoord(normalized.z)
        );
    }

    static uint ToVoxelCoord(float normalized) {
        const float scaled = std::clamp(normalized, 0.0f, 1.0f) * float(OCCUPANCY_GRID_DIM);
        return std::min(static_cast<uint>(std::floor(scaled)), OCCUPANCY_GRID_DIM - 1u);
    }
};

static_assert(ProbeGeometryClassifier::OCCUPANCY_VOXEL_COUNT <= 64u);

} // namespace Moer::Render::Raster
