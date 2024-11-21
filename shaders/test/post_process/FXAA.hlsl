/**
  * FXAA implementation
  * Reference: https://github.com/YXHXianYu/BJTU-Game-Engine/blob/main/engine/shader/glsl/final_fxaa.frag
  */

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

struct Constant {
    uint input_image;
    uint is_enable_fxaa;
};

[[vk::push_constant]] ConstantBuffer<Constant> param;

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {

    float3 input_image = TextureHandle(param.input_image).Sample2D<float3>(in_uv);

    if (!param.is_enable_fxaa) { return float4(input_image, 1.0); }

    float3 color1 = float3(in_uv.x, in_uv.y, 0.0);
    float3 color = lerp(color1, input_image, pow(in_uv.x, 0.1));

    return float4(color, 1.0);
}