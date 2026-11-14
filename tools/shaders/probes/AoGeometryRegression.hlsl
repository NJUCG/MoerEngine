#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/postprocess/lighting_effects/AoGeometry.hlsli"

static const float2 kRandomValues[7] = {
    float2(0.0, 0.0),
    float2(0.0, 0.25),
    float2(0.125, 0.5),
    float2(0.25, 0.75),
    float2(0.5, 1.0),
    float2(0.75, 0.999),
    float2(1.0, 0.625)
};

void CheckNormal(float3 normal, inout uint3 failures) {
    [unroll] for (uint i = 0; i < 7; ++i) {
        float2 random_values = kRandomValues[i];
        float3 direction = SampleAoCosineHemisphere(normal, random_values);
        float normal_projection = dot(direction, normal);
        float expected_projection = sqrt(1.0 - random_values.y);
        failures.x += abs(normal_projection - expected_projection) > 1e-5;
        failures.y += abs(dot(direction, direction) - 1.0) > 1e-5;
        failures.z += normal_projection < -1e-5;
    }
}

float4 main() : SV_Target {
    uint3 failures = uint3(0, 0, 0);
    CheckNormal(float3(1.0, 0.0, 0.0), failures);
    CheckNormal(float3(-1.0, 0.0, 0.0), failures);
    CheckNormal(float3(0.0, 1.0, 0.0), failures);
    CheckNormal(float3(0.0, -1.0, 0.0), failures);
    CheckNormal(float3(0.0, 0.0, 1.0), failures);
    CheckNormal(float3(0.0, 0.0, -1.0), failures);
    CheckNormal(normalize(float3(1.0, 2.0, 3.0)), failures);
    CheckNormal(normalize(float3(-3.0, 0.4, 2.0)), failures);
    return float4(failures, 56);
}
