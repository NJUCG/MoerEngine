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


float3 BilinearUpsample_1(float2 uv, float2 inSize, float2 outSize, uint input_image)
{
    // --- 1. 将高分输出UV映射回低分输入UV ---
    // 例如 inSize=960x540, outSize=1920x1080 → scale=0.5
    //float2 inUV = uv * (inSize / outSize);

    // --- 2. 转为输入图像像素空间 ---
    float2 texPos = uv * inSize - float2(0.5, 0.5);
    float2 base   = floor(texPos);  // 左上角像素坐标
    float2 fracUV = frac(texPos);   // 小数部分 (0~1)，用于插值

    // --- 3. 计算采样UV坐标 ---
    float2 uv00 = (base + float2(0.5, 0.5)) / inSize; // 左上
    float2 uv10 = (base + float2(1.5, 0.5)) / inSize; // 右上
    float2 uv01 = (base + float2(0.5, 1.5)) / inSize; // 左下
    float2 uv11 = (base + float2(1.5, 1.5)) / inSize; // 右下

    // --- 4. 从低分图采样颜色 ---
    TextureHandle tex = TextureHandle(input_image);
    float3 c00 = tex.SampleLevel(uv00, 0).rgb;
    float3 c10 = tex.SampleLevel(uv10, 0).rgb;
    float3 c01 = tex.SampleLevel(uv01, 0).rgb;
    float3 c11 = tex.SampleLevel(uv11, 0).rgb;

    // --- 5. 双线性插值 ---
    float3 cx0 = lerp(c00, c10, fracUV.x);
    float3 cx1 = lerp(c01, c11, fracUV.x);
    float3 color = lerp(cx0, cx1, fracUV.y);

    return color;
}

float3 DepthGuidedUpsample(TextureHandle lowTex, TextureHandle highDepth, float2 uv, float2 pixelSize)
{
    // 当前像素对应的高分深度
    float depthCenter = highDepth.SampleLevel(uv, 0).r;
    float3 colorCenter = lowTex.SampleLevel(uv, 0).rgb;

    float3 colorSum = 0;
    float weightSum = 0;

    const int radius = 1; // 小邻域 (3x3)
    for (int y = -radius; y <= radius; ++y)
    {
        for (int x = -radius; x <= radius; ++x)
        {
            float2 offset = float2(x, y) * pixelSize;
            float3 c = lowTex.SampleLevel(uv + offset, 0).rgb;
            float d = highDepth.SampleLevel(uv + offset, 0).r;

            // 权重 = 深度差 + 距离差
            float wDepth = exp(-abs(d - depthCenter) * 50.0);
            float wSpatial = exp(-dot(offset, offset) * 100.0);
            float w = wDepth * wSpatial;

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
    float3 precolor = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;
    //printf("color before upsample: %f, %f, %f, %f, %f\n", uv.x, uv.y, color.r, color.g, color.b);
    //printf("uv: %f, %f\n", uv.x * 540, uv.y * 960);

    if (param.upsample_mode == 0)
        color = BilinearUpsample(uv, 540, 1080, param.input_image);
        //color = DepthGuidedUpsample(lowTex, TextureHandle(param.high_res_depth), uv, param.inv_high_res);
    else if(param.upsample_mode == 1)
        color = BilinearUpsample_1(uv, param.inSize, param.outSize, param.input_image);

    // 锐化控制（可选）
    //color = pow(color, param.sharpness);
    if(precolor.r != color.r || precolor.g != color.g || precolor.b != color.b)
    {
        printf("NaN detected at uv: %f, %f, precolor: %f, %f, %f, color: %f, %f, %f mode:%d\n", uv.x, uv.y, precolor.r, precolor.g, precolor.b, color.r, color.g, color.b, param.upsample_mode);
    }
    //printf("color after upsample: %f, %f, %f, %f, %f\n", uv.x, uv.y, color.r, color.g, color.b);

    return float4(color, 1.0);
}
