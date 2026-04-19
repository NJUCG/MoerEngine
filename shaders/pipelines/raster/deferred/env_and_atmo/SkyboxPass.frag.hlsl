#ifndef RASTER_SKYBOX_HLSLI
#define RASTER_SKYBOX_HLSLI

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3)
#include "pipelines/RasterCommon.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::SkyboxPassBindlessParam> param;

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    float3 in_pos       = WorldPosFromDepth(0.99, in_uv, param.clip2world);
    float3 view_dir     = normalize(in_pos - param.camera_pos);
    float3 skybox_color = TextureHandle(param.cubemap_handle).SampleCube<float3>(view_dir);
    skybox_color *= param.exposure_factor;
    return float4(skybox_color, 1.0);
}

#endif // RASTER_SKYBOX_HLSLI