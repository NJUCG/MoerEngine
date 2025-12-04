/**
  * FXAA implementation
  * Reference: https://github.com/YXHXianYu/BJTU-Game-Engine/blob/main/engine/shader/glsl/final_fxaa.frag
  *
  * TODO: An possible optimization, precompute luminance per pixel and store it.
  *       In current implementation, luminance per pixel will be computed multiple times (10x or more!)
  */

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::FxaaPrecomputePipelineBindlessParam> param;

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    float3 color = TextureHandle(param.input_image).Sample2D<float3>(in_uv);

    float luminance = color.r * 0.213 + color.g * 0.715 + color.b * 0.072;

    return float4(color, luminance);
}