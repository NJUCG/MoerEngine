#ifndef MOER_AO_HISTORY_HLSLI
#define MOER_AO_HISTORY_HLSLI

#include "core/common/Common.hlsl"
#include "shared/utils/Packing.h"

// Integer storage preserves world-position bits. Normal alpha is an explicit validity byte.
uint4 PackAoGeometry(float3 world_position, float3 normal) {
    return uint4(asuint(world_position), Moer::Pack_R8G8B8A8_UFLOAT(float4(normal * 0.5 + 0.5, 1.0)));
}

bool IsAoGeometryValid(uint4 geometry) {
    return (geometry.w >> 24) == 255;
}

float3 AoGeometryPosition(uint4 geometry) {
    return asfloat(geometry.xyz);
}

float3 AoGeometryNormal(uint4 geometry) {
    return normalize(Moer::Unpack_R8G8B8A8_UFLOAT(geometry.w).xyz * 2.0 - 1.0);
}

bool IsAoHistoryGeometryValid(
    uint4 current,
    uint4 history,
    float3 camera_position,
    float relative_position_threshold,
    float normal_threshold,
    float pixel_footprint
) {
    if (!IsAoGeometryValid(current) || !IsAoGeometryValid(history)) {
        return false;
    }
    float3 position = AoGeometryPosition(current);
    float tolerance = max(max(relative_position_threshold, 0.0) * max(length(position - camera_position), 1.0),
                          1.5 * pixel_footprint);
    return length(position - AoGeometryPosition(history)) <= tolerance &&
           dot(AoGeometryNormal(current), AoGeometryNormal(history)) >= normal_threshold;
}

// Only same-plane neighbors estimate the footprint; another depth layer must not inflate it.
float AoNeighborFootprint(uint4 current, uint4 neighbor, float normal_threshold) {
    if (!IsAoGeometryValid(neighbor) ||
        dot(AoGeometryNormal(current), AoGeometryNormal(neighbor)) < normal_threshold) {
        return 1e30;
    }
    float3 delta = AoGeometryPosition(neighbor) - AoGeometryPosition(current);
    float distance = length(delta);
    return abs(dot(delta, AoGeometryNormal(current))) <= 0.25 * distance ? distance : 1e30;
}

#endif
