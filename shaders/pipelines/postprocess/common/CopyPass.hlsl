#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/ShaderParameters.h"

DEFINE_SHADER_CONSTANT(Moer::CopyPassBindlessParam, param)

float main(float2 uv : TEXCOORD0) : SV_TARGET {
    return TextureHandle(moer_constants.param.input_image).Sample2D<float>(uv);
}
