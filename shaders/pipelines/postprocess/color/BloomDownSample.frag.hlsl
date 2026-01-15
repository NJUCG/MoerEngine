#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"

// 确保绑定槽位一致
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::BloomDownsampleBindlessParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    TextureHandle src_tex = TextureHandle(param.downsample_chain_hdl);

    // 直接使用从 C++ 传进来的 1.0 / width 和 1.0 / height
    float x = param.inv_size.x;
    float y = param.inv_size.y;

    // 13-tap 采样模式
    // 这种模式通过 5 组不同的采样点组合，能够获得非常平滑的下采样结果

    // 采样中心和周围的 13 个点
    // 利用 Sample2D<float4> 配合 Linear Sampler 自动执行双线性插值
    float3 a = src_tex.Sample2D<float4>(uv + float2(-2 * x, 2 * y)).rgb;
    float3 b = src_tex.Sample2D<float4>(uv + float2(0, 2 * y)).rgb;
    float3 c = src_tex.Sample2D<float4>(uv + float2(2 * x, 2 * y)).rgb;

    float3 d = src_tex.Sample2D<float4>(uv + float2(-2 * x, 0)).rgb;
    float3 e = src_tex.Sample2D<float4>(uv + float2(0, 0)).rgb;
    float3 f = src_tex.Sample2D<float4>(uv + float2(2 * x, 0)).rgb;

    float3 g = src_tex.Sample2D<float4>(uv + float2(-2 * x, -2 * y)).rgb;
    float3 h = src_tex.Sample2D<float4>(uv + float2(0, -2 * y)).rgb;
    float3 i = src_tex.Sample2D<float4>(uv + float2(2 * x, -2 * y)).rgb;

    float3 j = src_tex.Sample2D<float4>(uv + float2(-x, y)).rgb;
    float3 k = src_tex.Sample2D<float4>(uv + float2(x, y)).rgb;
    float3 l = src_tex.Sample2D<float4>(uv + float2(-x, -y)).rgb;
    float3 m = src_tex.Sample2D<float4>(uv + float2(x, -y)).rgb;

    // 加权平均计算 (权重总和为 1.0)
    float3 result = e * 0.125;
    result += (a + c + g + i) * 0.03125;
    result += (b + d + f + h) * 0.0625;
    result += (j + k + l + m) * 0.125;

    // 确保结果不为负，且给一个极小的保底值防止后续计算出现 NaN
    return float4(max(result, 0.0001), 1.0);
}