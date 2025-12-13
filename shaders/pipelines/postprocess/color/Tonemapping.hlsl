#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;


// 注：Gamma矫正使用硬件sRGB实现，不需要在Shader中手动进行Gamma矫正
float4 main(float2 uv : TEXCOORD0) : SV_TARGET {

    float3 hdr_color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;

    hdr_color *= exp2(param.exposure_ev);

    float3 color = (param.reinhard_enabled ?  hdr_color / (hdr_color + 1.0) : hdr_color);

    return float4(color, 1.0);
}