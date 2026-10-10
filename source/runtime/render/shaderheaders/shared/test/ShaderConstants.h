#pragma once

#ifdef __cplusplus
#include "misc/Traits.h"
#include "shaderheaders/shared/ShaderConstants.h"
namespace Moer::Render {
#else
#include "shared/ShaderConstants.h"
namespace Moer {
#endif

struct ConstantTestData {
    uint left;
    uint right;
};

} // namespace Moer / Moer::Render

#define SHADER_TEST_CONSTANTS(X) \
    X(float, scale)             \
    X(ConstantTestData, data)   \
    X(float3, tint)             \
    X(float4x4, transform)      \
    X(uint, bias)
