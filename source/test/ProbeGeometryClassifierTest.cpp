#include "renderer/raster/ProbeGeometryClassifier.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

using Moer::Box3D;
using Moer::float3;
using Moer::Render::RASTER_PROBE_MAX_SUBDIVISION_LEVEL;
using Moer::Render::Raster::ProbeGeometryClassifier;

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1e-6f;
}

} // namespace

int main() {
    const Box3D cell_bounds(float3(0.0f), float3(8.0f));

    {
        const std::array<Box3D, 0> geometry{};
        const auto stats = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.0f, 0.25f, 8u);
        Expect(stats.intersecting_primitive_count == 0u, "Empty cell must not contain primitives.");
        Expect(stats.occupied_voxel_count == 0u, "Empty cell must not occupy voxels.");
        Expect(
            stats.desired_subdivision_level == RASTER_PROBE_MAX_SUBDIVISION_LEVEL,
            "Empty cell must request the coarsest level."
        );
    }

    {
        const std::array geometry = {Box3D(float3(0.1f), float3(1.0f))};
        const auto stats = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.0f, 0.25f, 8u);
        Expect(stats.intersecting_primitive_count == 1u, "Small geometry must intersect the cell.");
        Expect(stats.occupied_voxel_count == 1u, "Small geometry must occupy one analysis voxel.");
        Expect(stats.desired_subdivision_level == 1u, "Sparse geometry must request the middle level.");
        Expect(
            NearlyEqual(stats.occupancy, 1.0f / 64.0f),
            "Sparse geometry occupancy ratio must match the 4x4x4 grid."
        );
    }

    {
        const std::array geometry = {Box3D(float3(0.0f), float3(8.0f))};
        const auto stats = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.0f, 0.25f, 8u);
        Expect(stats.occupied_voxel_count == 64u, "Full geometry must occupy the complete analysis grid.");
        Expect(stats.desired_subdivision_level == 0u, "Dense geometry must request the finest level.");
    }

    {
        const std::array geometry = {
            Box3D(float3(8.10f, 0.1f, 0.1f), float3(8.20f, 1.0f, 1.0f))
        };
        const auto without_padding = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.0f, 0.25f, 8u);
        const auto with_padding = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.20f, 0.25f, 8u);
        Expect(
            without_padding.intersecting_primitive_count == 0u,
            "Out-of-cell geometry must not intersect without padding."
        );
        Expect(
            with_padding.intersecting_primitive_count == 1u,
            "Geometry padding must include nearby surfaces."
        );
    }

    {
        std::array<Box3D, 8> geometry{};
        for (Box3D& bound : geometry) {
            bound = Box3D(float3(0.1f), float3(1.0f));
        }
        const auto stats = ProbeGeometryClassifier::Analyze(cell_bounds, geometry, 0.0f, 0.25f, 8u);
        Expect(stats.occupied_voxel_count == 1u, "Overlapping detail must remain spatially sparse.");
        Expect(
            stats.desired_subdivision_level == 0u,
            "High primitive complexity must retain the finest level even with low occupancy."
        );
    }

    {
        const Box3D volume_bounds(float3(0.0f), float3(15.0f));
        const Box3D first = ProbeGeometryClassifier::BuildCellInfluenceBounds(
            float3(0.0f),
            float3(7.0f),
            float3(1.0f),
            volume_bounds
        );
        const Box3D second = ProbeGeometryClassifier::BuildCellInfluenceBounds(
            float3(8.0f, 0.0f, 0.0f),
            float3(7.0f, 15.0f, 15.0f),
            float3(1.0f),
            volume_bounds
        );
        Expect(NearlyEqual(first.min.x, 0.0f), "First cell influence must clamp to the volume minimum.");
        Expect(NearlyEqual(first.max.x, 7.5f), "First cell influence must end at the cell midpoint.");
        Expect(NearlyEqual(second.min.x, 7.5f), "Adjacent cell influences must meet without a gap.");
        Expect(NearlyEqual(second.max.x, 15.0f), "Last cell influence must clamp to the volume maximum.");

        const Box3D partial_tail = ProbeGeometryClassifier::BuildCellInfluenceBounds(
            float3(15.0f, 0.0f, 0.0f),
            float3(0.0f, 15.0f, 15.0f),
            float3(1.0f),
            volume_bounds
        );
        Expect(
            NearlyEqual(partial_tail.min.x, 14.5f),
            "A partial tail cell with one probe must retain its half-spacing influence."
        );
        Expect(
            NearlyEqual(partial_tail.max.x, 15.0f),
            "A partial tail cell influence must clamp to the volume maximum."
        );

        const Box3D singleton_axis = ProbeGeometryClassifier::BuildCellInfluenceBounds(
            float3(0.0f),
            float3(0.0f, 15.0f, 15.0f),
            float3(15.0f, 1.0f, 1.0f),
            volume_bounds
        );
        Expect(
            NearlyEqual(singleton_axis.min.x, 0.0f) && NearlyEqual(singleton_axis.max.x, 15.0f),
            "A globally singleton probe axis must cover the complete volume axis."
        );
    }

    std::cout << "ProbeGeometryClassifier tests passed." << std::endl;
    return EXIT_SUCCESS;
}
