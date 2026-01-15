#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"

// 确保绑定槽位一致
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::BloomUpsampleBindlessParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    TextureHandle upsample_tex   = TextureHandle(param.upsample_chain_hdl);
    TextureHandle downsample_tex = TextureHandle(param.downsample_chain_hdl);

    // 1. 使用从 C++ 传进来的小图（upsample_chain_hdl）的像素尺寸
    // 采样半径控制
    float x = param.inv_size.x * param.filter_radius;
    float y = param.inv_size.y * param.filter_radius;

    // 2. 对下一层级（小图）进行 9-tap Tent Filter 上采样
    // [1][2][1]
    // [2][4][2] * 1/16
    // [1][2][1]
    float3 upsampled;

    // 四角 (权重 1)
    upsampled = upsample_tex.Sample2D<float4>(uv + float2(-x, -y)).rgb * 1.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(x, -y)).rgb * 1.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(-x, y)).rgb * 1.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(x, y)).rgb * 1.0;

    // 十字 (权重 2)
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(0, -y)).rgb * 2.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(0, y)).rgb * 2.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(-x, 0)).rgb * 2.0;
    upsampled += upsample_tex.Sample2D<float4>(uv + float2(x, 0)).rgb * 2.0;

    // 中心 (权重 4)
    upsampled += upsample_tex.Sample2D<float4>(uv).rgb * 4.0;

    upsampled *= 0.0625; // 乘以 1/16

    // 3. 读取当前层级的原始降采样图 (来自 Downsample 链)
    float3 current = downsample_tex.Sample2D<float4>(uv).rgb;

    // 4. 融合：当前层细节 + 下一层放大的光晕
    float3 result = current + upsampled;

    return float4(result, 1.0);
}