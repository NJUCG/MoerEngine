#pragma once

#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

namespace Moer::Render::Raster {

struct ProbeBrickVirtualKey {
    uint  volume_config_index = RASTER_PROBE_PAGE_INVALID;
    uint  subdivision_level   = 0;
    uint3 coord                = uint3(0u);

    constexpr bool operator==(const ProbeBrickVirtualKey& other) const {
        return volume_config_index == other.volume_config_index &&
               subdivision_level == other.subdivision_level &&
               coord.x == other.coord.x && coord.y == other.coord.y && coord.z == other.coord.z;
    }

    constexpr bool IsValid() const {
        return volume_config_index < RASTER_PROBE_VOLUME_MAX_COUNT &&
               subdivision_level <= RASTER_PROBE_MAX_SUBDIVISION_LEVEL;
    }
};

class ProbeAdaptiveLayout {
public:
    static constexpr uint GetLevelGridDim(uint subdivision_level) {
        if (subdivision_level > RASTER_PROBE_MAX_SUBDIVISION_LEVEL) {
            return 0u;
        }
        const uint dim = RASTER_PROBE_MAX_FINE_BRICKS_PER_AXIS >> subdivision_level;
        return dim > 0u ? dim : 1u;
    }

    static constexpr uint GetLevelPageOffset(uint subdivision_level) {
        if (subdivision_level > RASTER_PROBE_MAX_SUBDIVISION_LEVEL) {
            return RASTER_PROBE_PAGE_INVALID;
        }

        uint page_offset = 0u;
        for (uint level = 0u; level < subdivision_level; ++level) {
            const uint dim = GetLevelGridDim(level);
            page_offset += dim * dim * dim;
        }
        return page_offset;
    }

    static uint GetVirtualPageIndex(const ProbeBrickVirtualKey& key) {
        if (!key.IsValid()) {
            return RASTER_PROBE_PAGE_INVALID;
        }

        const uint dim = GetLevelGridDim(key.subdivision_level);
        if (key.coord.x >= dim || key.coord.y >= dim || key.coord.z >= dim) {
            return RASTER_PROBE_PAGE_INVALID;
        }

        const uint local_page = GetLevelPageOffset(key.subdivision_level) + key.coord.x +
                                key.coord.y * dim + key.coord.z * dim * dim;
        return key.volume_config_index * RASTER_PROBE_MAX_PAGES_PER_VOLUME + local_page;
    }

    static ProbeBrickVirtualKey GetParent(const ProbeBrickVirtualKey& key) {
        if (!key.IsValid() || key.subdivision_level >= RASTER_PROBE_MAX_SUBDIVISION_LEVEL) {
            return {};
        }
        return ProbeBrickVirtualKey{
            key.volume_config_index,
            key.subdivision_level + 1u,
            uint3(key.coord.x / 2u, key.coord.y / 2u, key.coord.z / 2u)
        };
    }

    static uint GetParentPageIndex(const ProbeBrickVirtualKey& key) {
        return GetVirtualPageIndex(GetParent(key));
    }

    static uint GetNeighborPageIndex(
        const ProbeBrickVirtualKey& key,
        int                         offset_x,
        int                         offset_y,
        int                         offset_z,
        uint3                       active_level_counts
    ) {
        if (!key.IsValid()) {
            return RASTER_PROBE_PAGE_INVALID;
        }

        const int neighbor_x = static_cast<int>(key.coord.x) + offset_x;
        const int neighbor_y = static_cast<int>(key.coord.y) + offset_y;
        const int neighbor_z = static_cast<int>(key.coord.z) + offset_z;
        if (neighbor_x < 0 || neighbor_y < 0 || neighbor_z < 0 ||
            neighbor_x >= static_cast<int>(active_level_counts.x) ||
            neighbor_y >= static_cast<int>(active_level_counts.y) ||
            neighbor_z >= static_cast<int>(active_level_counts.z)) {
            return RASTER_PROBE_PAGE_INVALID;
        }

        ProbeBrickVirtualKey neighbor = key;
        neighbor.coord = uint3(
            static_cast<uint>(neighbor_x),
            static_cast<uint>(neighbor_y),
            static_cast<uint>(neighbor_z)
        );
        return GetVirtualPageIndex(neighbor);
    }

    static uint3 GetCellCounts(uint3 fine_brick_counts) {
        constexpr uint cell_dim = RASTER_PROBE_CELL_BRICK_DIM;
        return uint3(
            (fine_brick_counts.x + cell_dim - 1u) / cell_dim,
            (fine_brick_counts.y + cell_dim - 1u) / cell_dim,
            (fine_brick_counts.z + cell_dim - 1u) / cell_dim
        );
    }

    static uint3 GetCellCoord(uint3 fine_brick_coord) {
        constexpr uint cell_dim = RASTER_PROBE_CELL_BRICK_DIM;
        return uint3(
            fine_brick_coord.x / cell_dim,
            fine_brick_coord.y / cell_dim,
            fine_brick_coord.z / cell_dim
        );
    }
};

static_assert(
    ProbeAdaptiveLayout::GetLevelGridDim(RASTER_PROBE_MAX_SUBDIVISION_LEVEL) == 1u,
    "The coarsest Probe Brick level must collapse to a single root page."
);
static_assert(
    ProbeAdaptiveLayout::GetLevelPageOffset(RASTER_PROBE_MAX_SUBDIVISION_LEVEL) + 1u <=
        RASTER_PROBE_MAX_PAGES_PER_VOLUME,
    "The per-volume virtual page range is too small for the Probe Brick hierarchy."
);

} // namespace Moer::Render::Raster
