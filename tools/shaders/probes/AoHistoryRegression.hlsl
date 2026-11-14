#include "pipelines/postprocess/lighting_effects/AoHistory.hlsli"

struct RegressionOutput {
    float4 geometry : SV_Target0;
    float4 footprint : SV_Target1;
};

RegressionOutput main() {
    RegressionOutput output;
    uint4 current = PackAoGeometry(float3(0, 0, 10), float3(0, 0, -1));
    uint4 other_layer = PackAoGeometry(float3(0, 0, 5), float3(0, 0, -1));
    uint4 other_normal = PackAoGeometry(float3(0, 0, 10), float3(0, 1, 0));
    output.geometry = float4(
        IsAoHistoryGeometryValid(current, current, float3(3, 2, 1), 0.001, 0.9, 0.1),
        !IsAoHistoryGeometryValid(current, other_layer, float3(0, 0, 0), 0.001, 0.9, 0.1),
        !IsAoHistoryGeometryValid(current, other_normal, float3(0, 0, 0), 0.001, 0.9, 0.1),
        !IsAoHistoryGeometryValid(current, uint4(0, 0, 0, 0), float3(0, 0, 0), 0.001, 0.9, 0.1)
    );

    uint4 neighbor = PackAoGeometry(float3(0.1, 0, 10), float3(0, 0, -1));
    float footprint = AoNeighborFootprint(current, neighbor, 0.9);
    output.footprint = float4(
        IsAoHistoryGeometryValid(current, neighbor, float3(0, 0, 0), 0.0, 0.9, footprint),
        !IsAoHistoryGeometryValid(current, PackAoGeometry(float3(0.3, 0, 10), float3(0, 0, -1)),
                                  float3(0, 0, 0), 0.0, 0.9, footprint),
        AoNeighborFootprint(current, other_layer, 0.9) > 1e29,
        IsAoGeometryValid(current) && dot(AoGeometryNormal(current), float3(0, 0, -1)) > 0.999
    );
    return output;
}
