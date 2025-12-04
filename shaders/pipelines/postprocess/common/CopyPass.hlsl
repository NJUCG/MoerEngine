#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::CopyPassBindlessParam> param;

float main(float2 uv : TEXCOORD0) : SV_TARGET {
    return TextureHandle(param.input_image).Sample2D<float>(uv);
}