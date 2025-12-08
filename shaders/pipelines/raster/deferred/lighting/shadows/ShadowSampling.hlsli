#ifndef MOER_SHADOW_SAMPLING_HLSLI
#define MOER_SHADOW_SAMPLING_HLSLI

#include "core/math/Math.hlsli" // 确保包含 PI 等常量

// =================================================================================================
// 采样模式数据 (Poisson Disk)
// =================================================================================================

static const float2 POISSON_DISK_16[16] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 ),
    float2(  0.44323325, -0.97511554 ), float2(  0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ), float2(  0.79197514,  0.19090188 ),
    float2( -0.24188840,  0.99706507 ), float2( -0.81409955,  0.91437590 ),
    float2(  0.19984126,  0.78641367 ), float2(  0.14383161, -0.14100790 )
};

// =================================================================================================
// 随机数生成
// =================================================================================================

// 简单的 Interleaved Gradient Noise 或 Hash
float nrand(float2 uv) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

// 生成随机旋转矩阵 (用于消除带状伪影)
float2x2 GetRandomRotation(float2 screenUV) {
    float theta = nrand(screenUV) * 2.0 * PI;
    float c = cos(theta);
    float s = sin(theta);
    return float2x2(c, -s, s, c);
}

// =================================================================================================
// 矩阵变换工具 (用于 PCSS Method 2)
// =================================================================================================

// 生成一个沿特定方向缩放的 2x2 矩阵
// Direction: 缩放的主轴方向 (必须归一化)
// ScaleMinusOne: (缩放倍数 - 1)
float2x2 GenerateDirectionalScale2x2Matrix(float2 Direction, float ScaleMinusOne)
{
    // 构造矩阵 M = I + s * v * v^T
    return float2x2(
       1.0 + ScaleMinusOne * Direction.x * Direction.x, ScaleMinusOne * Direction.y * Direction.x,
       ScaleMinusOne * Direction.x * Direction.y,       1.0 + ScaleMinusOne * Direction.y * Direction.y
    );
}

// =================================================================================================
// 采样辅助
// =================================================================================================

// 获取采样点 (带旋转)
float2 GetSampleOffset(int index, float radius, float2x2 rotation) {
    return mul(rotation, POISSON_DISK_16[index]) * radius;
}

#endif