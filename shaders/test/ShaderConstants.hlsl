#include "shared/test/ShaderConstants.h"

DEFINE_SHADER_CONSTANTS(SHADER_TEST_CONSTANTS)
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> values;

[numthreads(1, 1, 1)]
void ComputeMain(uint3 index : SV_DispatchThreadID) {
    values[index.x] = float4(
        moer_constants.scale + moer_constants.data.left + moer_constants.bias,
        moer_constants.tint.x,
        moer_constants.tint.z,
        moer_constants.transform[0][0] + moer_constants.data.right);
}

float4 VertexMain(uint index : SV_VertexID) : SV_Position {
    const float2 corners[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return mul(moer_constants.transform, float4(corners[index] * moer_constants.scale, 0, 1));
}

float4 PixelMain() : SV_Target {
    return float4(moer_constants.tint * moer_constants.scale,
                  float(moer_constants.data.left + moer_constants.bias));
}
