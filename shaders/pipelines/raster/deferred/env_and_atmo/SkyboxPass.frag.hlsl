#ifndef RASTER_SKYBOX_HLSLI
#define RASTER_SKYBOX_HLSLI

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/RasterCommon.hlsli"
#include "shared/raster/ShaderParameters.h"

DEFINE_SHADER_CONSTANT(Moer::SkyboxPassBindlessParam, param)

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    float3 in_pos       = WorldPosFromDepth(0.99, in_uv, moer_constants.param.clip2world);
    float3 view_dir     = normalize(in_pos - moer_constants.param.camera_pos);
    float3 skybox_color = TextureHandle(moer_constants.param.cubemap_handle).SampleCube<float3>(view_dir);
    skybox_color *= moer_constants.param.exposure_factor;
    return float4(skybox_color, 1.0);
}

#endif // RASTER_SKYBOX_HLSLI
