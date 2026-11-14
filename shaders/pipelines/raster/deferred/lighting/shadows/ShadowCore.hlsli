#ifndef MOER_SHADOW_CORE_HLSLI
#define MOER_SHADOW_CORE_HLSLI

#include "core/common/Bindless.hlsl"

// =================================================================================================
// 数据结构定义
// =================================================================================================

// Shader-local state only; CPU upload layouts remain in LightingData.
struct DirectionalShadowContext {
    uint   shadowMapHandle;
    float2 shadowUV;
    float  receiverDepthNdc; // Orthographic reverse-Z depth.
    float2 screenUV;
    float3 normal;
    float3 lightDir;
    float  lightAngleTangent; // tan(light angular radius), not a world-space size.
    float  shadowMapSize;
    float  orthographicWidth;
    float  depthRangeWorld;
};

struct PointShadowContext {
    uint   shadowMapHandle;
    float  receiverFaceDepthWorld; // max(abs(worldPos - lightPos)), not radial distance.
    float2 screenUV;
    float3 normal;
    float3 lightDir;
    float3 tangent;
    float3 bitangent;
    float  lightRadiusWorld;
    float  shadowMapSize;
    float  nearPlaneWorld;
    float  farPlaneWorld;
};

struct BlockerStats {
    uint blockerCase;//0:no blocker, 1:half blocker, 2:full blocker
    float avgDepth;
    float numBlockers;
    float2 uvSum;       // sum(x), sum(y)
    float2 uvSqSum;     // sum(x^2), sum(y^2)
    float uvCrossSum;   // sum(x*y)
};



// =================================================================================================
// 硬件指令封装 (Quad Sharing)
// =================================================================================================

// 标量版本
float QuadAverage(float val) {
    float val0 = QuadReadLaneAt(val, 0);
    float val1 = QuadReadLaneAt(val, 1);
    float val2 = QuadReadLaneAt(val, 2);
    float val3 = QuadReadLaneAt(val, 3);
    return (val0 + val1 + val2 + val3) * 0.25;
}

// 向量版本
float2 QuadAverage(float2 val) {
    return float2(QuadAverage(val.x), QuadAverage(val.y));
}

// =================================================================================================
// 基础阴影逻辑
// =================================================================================================

// 参考 UE: 基于斜率的 Bias 计算
float GetSlopeScaledBias(float3 normal, float3 lightDir) {
    float cosTheta = saturate(dot(normal, -lightDir));
    // tan(acos(x)) == sqrt(1 - x*x) / x，避免两个超越函数
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float bias = 0.005 * sinTheta / max(cosTheta, 1e-4);
    return clamp(bias, 0.0001, 0.01);
}

// Callers supply the bias policy so this helper does not depend on PCSS configuration macros.
float GetShadowDepthBias(
    float3 normal,
    float3 lightDir,
    float fixedBias,
    bool dynamicBiasEnabled,
    bool maxBiasEnabled
) {
    float bias = fixedBias;
    if (dynamicBiasEnabled) {
        bias = GetSlopeScaledBias(normal, lightDir);
        if (maxBiasEnabled) {
            bias = min(bias, fixedBias);
        }
    }
    return bias;
}

void GetTangentBasis(float3 N, out float3 T, out float3 B) {
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(Up, N));
    B = cross(N, T);
}

#endif
