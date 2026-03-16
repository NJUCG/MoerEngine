#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "materials/Material.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::DofPipelineBindlessParam> param;

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {
    // uv 即 屏幕坐标，值域为[0, 1]，表示了不同的像素

    float3 color       = TextureHandle(param.input_color_tex).Sample2D<float4>(uv).rgb;
    float3 debug_color = float3(1.0, 0.0, 1.0);

    color = lerp(color, debug_color, param.debug_param);

    return float4(color, 1.0);
}