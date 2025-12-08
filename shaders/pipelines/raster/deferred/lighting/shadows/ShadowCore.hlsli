#ifndef MOER_SHADOW_CORE_HLSLI
#define MOER_SHADOW_CORE_HLSLI

#include "core/common/Bindless.hlsl"

// =================================================================================================
// 数据结构定义
// =================================================================================================

// 统一的阴影计算上下文
struct ShadowContext {
    // --- 资源 ---
    uint   shadowMapHandle; // ShadowMap Bindless ID

    // --- 坐标 ---
    float2 shadowUV;        // 当前像素在 ShadowMap 上的 UV
    float  fragmentDepth;   // 当前像素的深度 (Receiver Depth)
    float2 screenUV;        // 屏幕空间 UV (用于生成随机数)

    // --- 光源与投影参数 ---
    float  lightSizeWorld;  // 光源物理大小
    float  shadowMapSize;   // ShadowMap 分辨率 (如 2048)
    float  clipW;           // 投影矩阵 W 分量 (用于透视校正)

    // 用于计算 Slope Bias
    float3 normal;    // 世界空间法线
    float3 lightDir;  // 光线传播的方向
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
    float bias = 0.005 * tan(acos(cosTheta)); // 简单近似
    return clamp(bias, 0.0001, 0.01);
}

#endif