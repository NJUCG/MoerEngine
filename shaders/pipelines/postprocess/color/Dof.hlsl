#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "materials/Material.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::DofPipelineBindlessParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    // uv 即 屏幕坐标，值域为[0, 1]，表示了不同的像素

    // 上一个阶段、对应位置像素的颜色
    float3 color = TextureHandle(param.input_color_tex).Sample2D<float4>(uv).rgb;

    // ==============================
    // TODO(lab2-dof): 实现后处理Shader
    // ==============================

    return float4(color, 1.0);
}