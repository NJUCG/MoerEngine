#include "shared/test/ShaderConstants.h"

#define PIXEL_CONSTANTS(X)   \
    X(float, scale)         \
    X(ConstantTestData, data) \
    X(float3, tint)         \
    X(uint, bias)

DEFINE_SHADER_CONSTANTS(PIXEL_CONSTANTS)

float4 PixelMain() : SV_Target {
    return float4(moer_constants.tint * moer_constants.scale,
                  float(moer_constants.data.left + moer_constants.bias));
}
