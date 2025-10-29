#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
SamplerState gLinearClamp : register(s0);
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/post_process/ShaderParameters.h"

[[vk::push_constant]]
ConstantBuffer<Moer::UpsamplePipelineBindlessParam> param;


#define UPSAMPLE_DEPTH_MODE 0
#define UPSAMPLE_BILINEAR_MODE 1

float3 BicubicFilterNoCornersWithFallbackToBilinearFilterWithCustomWeights_Color(
    TextureHandle tex,
    float4 uv01,
    float4 uv23,
    float2 uv4,
    float4 w,
    float  w4,
    float  sum)
{
    float3 color = 0.0;

    // 五点采样（4个主方向点 + 1个中心）
    color  = tex.SampleLevel(uv01.xy, 0).rgb * w.x;
    color += tex.SampleLevel(uv01.zw, 0).rgb * w.y;
    color += tex.SampleLevel(uv23.xy, 0).rgb * w.z;
    color += tex.SampleLevel(uv23.zw, 0).rgb * w.w;
    color += tex.SampleLevel(uv4, 0).rgb * w4;

    // 归一化（避免除零）
    if (sum > 0.0001)
        color /= sum;
    else
        color = 0.0;

    return color;
}

// 简单的双线性采样
float3 BilinearUpsample(float2 uv, float2 inSize, float2 outSize, uint input_image)
{

    // 将当前输出像素的 UV 映射回输入图像的归一化 UV
    float2 inUV = uv * (inSize / outSize);

    // 计算输入图像中的像素坐标
    float2 texPos = inUV * inSize - 0.5;
    float2 base   = floor(texPos);         // 左上角像素坐标
    float2 fracUV = frac(texPos);          // 小数偏移，用于权重

    // ------------------------------
    // Bicubic 权重计算（Catmull-Rom 核）
    // ------------------------------
    float2 f = fracUV;
    float4 w;
    w.x = ((-0.5 * f.x + 1.0) * f.x - 0.5) * f.x;          // 左1
    w.y = ((1.5 * f.x - 2.5) * f.x) * f.x + 1.0;           // 左0
    w.z = ((-1.5 * f.x + 2.0) * f.x + 0.5) * f.x;          // 右1
    w.w = ((0.5 * f.x - 0.5) * f.x) * f.x;                 // 右2
    float w4 = 0.0;                                         // 中心点权重（可选）
    float sum = w.x + w.y + w.z + w.w + w4;                 // 总权重

    // ------------------------------
    // 计算采样 UV 坐标
    // ------------------------------
    float4 uv01;
    float4 uv23;
    float2 uv4;

    uv01.xy = (base + float2(-1.0, 0.5)) / inSize;  // 左1
    uv01.zw = (base + float2( 0.0, 0.5)) / inSize;  // 左0
    uv23.xy = (base + float2( 1.0, 0.5)) / inSize;  // 右1
    uv23.zw = (base + float2( 2.0, 0.5)) / inSize;  // 右2
    uv4     = (base + float2(0.5, 0.5)) / inSize;   // 中心点（可选）
    
    float3 color = BicubicFilterNoCornersWithFallbackToBilinearFilterWithCustomWeights_Color(TextureHandle(input_image), uv01, uv23, uv4, w, w4, sum);

    return color;
}

// 深度引导上采样（近似 bilateral）
float3 DepthGuidedUpsample(TextureHandle lowTex, TextureHandle highDepth, float2 uv, float2 pixelSize)
{
    float3 base = lowTex.Sample2D<float4>(uv).rgb;
    float baseDepth = TextureHandle(highDepth).Sample2D<float>(uv);

    float3 colorSum = 0;
    float weightSum = 0;

    const int radius = 1;
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            float2 offset = float2(x, y) * pixelSize;
            float3 c = lowTex.Sample2D<float4>(uv + offset).rgb;
            float d = TextureHandle(highDepth).Sample2D<float>(uv + offset);

            float w = exp(-abs(d - baseDepth) * 50.0); // 深度差权重
            colorSum += c * w;
            weightSum += w;
        }
    }

    return colorSum / max(weightSum, 1e-5);
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET
{
    //TextureHandle lowTex = TextureHandle(param.input_image);

    float3 color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;

    if (param.upsample_mode == 0)
        color = BilinearUpsample(uv, param.inSize, param.outSize, param.input_image);
        //color = DepthGuidedUpsample(lowTex, TextureHandle(param.high_res_depth), uv, param.inv_high_res);
    else if(param.upsample_mode == 1)
        color = BilinearUpsample(uv, param.inSize, param.outSize, param.input_image);

    // 锐化控制（可选）
    //color = pow(color, param.sharpness);

    return float4(color, 1.0);
}
