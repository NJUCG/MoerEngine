#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Texture2D input_image; 
[[vk::binding(1, 0)]] Buffer<uint> exposure;

// 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
float4 main(float2 uv : TEXCOORD0, float4 pos : SV_Position) : SV_TARGET {

    float3 hdr_color = input_image[floor(pos.xy)].rgb;

    hdr_color *= exp2(param.exposure_ev);

    float3 color = (param.reinhard_enabled ?  hdr_color / (hdr_color + 1.0) : hdr_color);

    if (uv.y < 0.5) {
        color = asfloat(exposure[0]).xxx;
    }

    return float4(color, 1.0);
}