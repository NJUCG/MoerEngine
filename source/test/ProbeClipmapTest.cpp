#include "renderer/raster/ProbeClipmap.h"

#include <cmath>
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

bool Equal(int3 lhs, int3 rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool Equal(uint3 lhs, uint3 rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1e-5f;
}

bool TestCellStepAndAnchorHysteresis() {
    const float3 extent(30.0f, 6.0f, 30.0f);
    const float3 base_origin(-15.0f, 0.0f, -15.0f);
    const float3 step = ProbeClipmap::GetCellStep(extent, uint3(16u, 4u, 16u));
    const ProbeClipmapAnchor initial = ProbeClipmap::ResolveAnchor(
        float3(0.0f, 20.0f, 0.0f),
        base_origin,
        extent,
        step,
        int3(0),
        false,
        false,
        0.1f
    );
    const ProbeClipmapAnchor held = ProbeClipmap::ResolveAnchor(
        float3(9.0f, 20.0f, 0.0f),
        base_origin,
        extent,
        step,
        initial.cell,
        true,
        false,
        0.1f
    );
    const ProbeClipmapAnchor moved = ProbeClipmap::ResolveAnchor(
        float3(11.0f, 20.0f, 0.0f),
        base_origin,
        extent,
        step,
        held.cell,
        true,
        false,
        0.1f
    );

    return Expect(NearlyEqual(step.x, 16.0f), "16 probes over 30 units must scroll by an eight-spacing Cell") &&
           Expect(Equal(initial.cell, int3(0)), "camera at configured center must preserve the base anchor") &&
           Expect(Equal(held.cell, int3(0)), "anchor must remain inside the hysteresis dead zone") &&
           Expect(Equal(moved.cell, int3(1, 0, 0)), "anchor must advance after crossing the dead zone") &&
           Expect(NearlyEqual(moved.origin.x, 1.0f), "runtime origin must advance by one Cell step") &&
           Expect(
               moved.cell.y == 0 && NearlyEqual(moved.origin.y, base_origin.y),
               "disabled Y follow must preserve configured height"
           );
}

bool TestSmallGridCoverageRemainsContinuous() {
    const float3 extent(16.0f, 6.0f, 16.0f);
    const float3 base_origin(-8.0f, 0.0f, -8.0f);
    const float3 step = ProbeClipmap::GetCellStep(extent, uint3(8u, 4u, 8u));
    const ProbeClipmapAnchor moved = ProbeClipmap::ResolveAnchor(
        float3(8.1f, 20.0f, 0.0f),
        base_origin,
        extent,
        step,
        int3(0),
        true,
        false,
        0.1f
    );

    return Expect(NearlyEqual(step.x, extent.x), "a sub-Cell axis must not scroll farther than its extent") &&
           Expect(
               moved.cell.x == 1 && moved.origin.x <= 8.1f,
               "axis hysteresis must not let the camera leave a non-overlapping Clipmap window"
           );
}

bool TestWorldBrickCoordinates() {
    return Expect(
               Equal(
                   ProbeClipmap::GetWorldFineBrickCoord(int3(3, -2, 1), uint3(1u, 0u, 3u)),
                   int3(7, -4, 5)
               ),
               "world Fine Brick coordinates must include the two-Brick Cell stride"
           ) &&
           Expect(
               Equal(
                   ProbeClipmap::GetWorldFineBrickCoord(int3(-1, 0, -2), uint3(0u, 1u, 1u)),
                   int3(-2, 1, -3)
               ),
               "negative Clipmap anchors must remain stable integer keys"
           ) &&
           Expect(
               Equal(
                   ProbeClipmap::GetWorldFineBrickCoord(int3(0, 0, 0), uint3(2u, 0u, 1u)),
                   ProbeClipmap::GetWorldFineBrickCoord(int3(1, 0, 0), uint3(0u, 0u, 1u))
               ),
               "overlapping L0 Bricks must retain their world key after one Cell scroll"
           );
}

bool TestMotionPrefetchNeighbor() {
    uint3 neighbor{};
    return Expect(
               Equal(
                   ProbeClipmap::GetDominantPrefetchOffset(float3(0.2f, 0.1f, -2.0f), 0.05f),
                   int3(0, 0, -1)
               ),
               "dominant negative Z motion must prefetch the -Z neighbor"
           ) &&
           Expect(
               Equal(
                   ProbeClipmap::GetDominantPrefetchOffset(float3(0.01f, 0.0f, 0.0f), 0.05f),
                   int3(0)
               ),
               "sub-threshold camera jitter must not generate prefetch"
           ) &&
           Expect(
               ProbeClipmap::ResolveNeighborCoord(
                   uint3(1u, 0u, 1u),
                   int3(1, 0, 0),
                   uint3(4u, 1u, 3u),
                   neighbor
               ) && Equal(neighbor, uint3(2u, 0u, 1u)),
               "in-bounds prefetch neighbor must resolve"
           ) &&
           Expect(
               !ProbeClipmap::ResolveNeighborCoord(
                   uint3(0u, 0u, 0u),
                   int3(-1, 0, 0),
                   uint3(4u, 1u, 3u),
                   neighbor
               ),
               "out-of-bounds prefetch neighbor must be rejected"
           );
}

} // namespace

int main() {
    if (!TestCellStepAndAnchorHysteresis() || !TestSmallGridCoverageRemainsContinuous() ||
        !TestWorldBrickCoordinates() ||
        !TestMotionPrefetchNeighbor()) {
        return 1;
    }
    std::cout << "Probe clipmap tests passed." << std::endl;
    return 0;
}
