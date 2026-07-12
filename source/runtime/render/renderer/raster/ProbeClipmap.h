#pragma once

#include "misc/Traits.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include <algorithm>
#include <cmath>

namespace Moer::Render::Raster {

struct ProbeClipmapAnchor {
    int3   cell   = int3(0);
    float3 origin = float3(0.0f);
    float3 step   = float3(1.0f);
};

class ProbeClipmap {
public:
    static float3 GetCellStep(float3 extent, uint3 probe_counts) {
        constexpr float cell_probe_stride =
            float(RASTER_PROBE_BRICK_DIM * RASTER_PROBE_CELL_BRICK_DIM);
        const float3 spacing(
            probe_counts.x > 1u ? extent.x / float(probe_counts.x - 1u) : extent.x,
            probe_counts.y > 1u ? extent.y / float(probe_counts.y - 1u) : extent.y,
            probe_counts.z > 1u ? extent.z / float(probe_counts.z - 1u) : extent.z
        );
        return float3(
            std::max(std::min(spacing.x * cell_probe_stride, extent.x), 1e-4f),
            std::max(std::min(spacing.y * cell_probe_stride, extent.y), 1e-4f),
            std::max(std::min(spacing.z * cell_probe_stride, extent.z), 1e-4f)
        );
    }

    static ProbeClipmapAnchor ResolveAnchor(
        float3 camera_position,
        float3 base_origin,
        float3 extent,
        float3 cell_step,
        int3   previous_cell,
        bool   has_previous,
        bool   follow_y,
        float  hysteresis_cells
    ) {
        const float hysteresis = std::clamp(hysteresis_cells, 0.0f, 0.49f);
        ProbeClipmapAnchor result{};
        result.step = cell_step;
        result.cell.x = ResolveAxisCell(
            camera_position.x,
            base_origin.x,
            extent.x,
            cell_step.x,
            previous_cell.x,
            has_previous,
            hysteresis
        );
        result.cell.y = 0;
        if (follow_y) {
            result.cell.y = ResolveAxisCell(
                camera_position.y,
                base_origin.y,
                extent.y,
                cell_step.y,
                previous_cell.y,
                has_previous,
                hysteresis
            );
        }
        result.cell.z = ResolveAxisCell(
            camera_position.z,
            base_origin.z,
            extent.z,
            cell_step.z,
            previous_cell.z,
            has_previous,
            hysteresis
        );
        result.origin = float3(
            base_origin.x + float(result.cell.x) * cell_step.x,
            base_origin.y + float(result.cell.y) * cell_step.y,
            base_origin.z + float(result.cell.z) * cell_step.z
        );
        return result;
    }

    static int3 GetWorldFineBrickCoord(int3 anchor_cell, uint3 local_brick_coord) {
        constexpr int cell_brick_stride = int(RASTER_PROBE_CELL_BRICK_DIM);
        return int3(
            anchor_cell.x * cell_brick_stride + int(local_brick_coord.x),
            anchor_cell.y * cell_brick_stride + int(local_brick_coord.y),
            anchor_cell.z * cell_brick_stride + int(local_brick_coord.z)
        );
    }

    static int3 GetDominantPrefetchOffset(float3 camera_motion, float motion_threshold) {
        const float threshold = std::max(motion_threshold, 0.0f);
        const float length_sq = camera_motion.x * camera_motion.x +
                                camera_motion.y * camera_motion.y +
                                camera_motion.z * camera_motion.z;
        if (length_sq < threshold * threshold || length_sq <= 1e-12f) {
            return int3(0);
        }

        const float abs_x = std::abs(camera_motion.x);
        const float abs_y = std::abs(camera_motion.y);
        const float abs_z = std::abs(camera_motion.z);
        if (abs_x >= abs_y && abs_x >= abs_z) {
            return int3(camera_motion.x >= 0.0f ? 1 : -1, 0, 0);
        }
        if (abs_y >= abs_z) {
            return int3(0, camera_motion.y >= 0.0f ? 1 : -1, 0);
        }
        return int3(0, 0, camera_motion.z >= 0.0f ? 1 : -1);
    }

    static bool ResolveNeighborCoord(uint3 coord, int3 offset, uint3 counts, uint3& neighbor) {
        const int x = int(coord.x) + offset.x;
        const int y = int(coord.y) + offset.y;
        const int z = int(coord.z) + offset.z;
        if (x < 0 || y < 0 || z < 0 || x >= int(counts.x) || y >= int(counts.y) ||
            z >= int(counts.z)) {
            return false;
        }
        neighbor = uint3(uint(x), uint(y), uint(z));
        return true;
    }

private:
    static int ResolveAxisCell(
        float camera_position,
        float base_origin,
        float extent,
        float cell_step,
        int   previous_cell,
        bool  has_previous,
        float hysteresis
    ) {
        const float desired_origin = camera_position - extent * 0.5f;
        const float safe_cell_step = std::max(cell_step, 1e-4f);
        const float desired_cell = (desired_origin - base_origin) / safe_cell_step;
        const float max_safe_hysteresis =
            std::max(extent / (2.0f * safe_cell_step) - 0.5f, 0.0f);
        const float axis_hysteresis = std::min(hysteresis, max_safe_hysteresis);
        if (has_previous &&
            std::abs(desired_cell - float(previous_cell)) <= 0.5f + axis_hysteresis) {
            return previous_cell;
        }
        return int(std::floor(desired_cell + 0.5f));
    }
};

} // namespace Moer::Render::Raster
