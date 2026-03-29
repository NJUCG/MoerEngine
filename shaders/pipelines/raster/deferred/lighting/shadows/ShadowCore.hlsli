#ifndef MOER_SHADOW_CORE_HLSLI
#define MOER_SHADOW_CORE_HLSLI

#include "core/common/Bindless.hlsl"

// =================================================================================================
// 数据结构定义
// =================================================================================================

// 统一的阴影计算上下文
struct ShadowContext {
    // =========================================================
    // 资源 (Common)
    // =========================================================
    uint   shadowMapHandle; // Bindless Texture ID

    // =========================================================
    // 坐标与采样 (Multiplexed)
    // =========================================================
    // [CSM]: 当前像素在 ShadowMap 上的 2D UV
    // [Point]: 未使用 (Point 使用 lightDir 进行 3D 采样)
    float2 shadowUV;        

    // [CSM]: 当前像素的 NDC 深度 (非线性)
    // [Point]: 当前像素到光源的 **线性距离** (World Distance) -> PCSS 比较需要线性空间
    float  fragmentDepth;   

    // [Common]: 屏幕空间 UV (用于随机数旋转)
    float2 screenUV;  

    // =========================================================
    // 几何与方向 (Common / Point)
    // =========================================================
    // [CSM]: 世界空间法线
    // [Point]: 世界空间法线 (用于 Bias)
    float3 normal;    

    // [CSM]: 平行光方向 (统一方向)
    // [Point]: 从光源指向像素的方向 (normalize(worldPos - lightPos))
    float3 lightDir;  

    // [Point Only]: 切线空间基向量 (用于扰动采样方向)
    // [CSM]: 未使用
    float3 Tangent;        
    float3 Bitangent;    

    // =========================================================
    // 光源与投影参数 (Common / Multiplexed)
    // =========================================================
    // [Common]: 光源物理大小 (CSM: World Size, Point: UV Size or World Radius?) 
    // 建议统一为 World Size，在 Penumbra 计算时转换
    float  lightSizeWorld;  

    // [Common]: ShadowMap 分辨率
    float  shadowMapSize;   

    // [CSM]: 投影矩阵 W 分量 (用于透视校正，CSM 正交通常为 1.0)
    // [Point]: 1.0 (Point 使用线性距离比较，无需透视除法)
    float  clipW;           

    // [CSM]: (OrthoWidth, OrthoHeight, ZRange, Near)
    // [Point]: (NearPlane, FarPlane, 0, 0) -> 用于 Reverse-Z 线性化
    float4 scaleData;  
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

void GetTangentBasis(float3 N, out float3 T, out float3 B) {
    float3 Up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(Up, N));
    B = cross(N, T);
}

#endif