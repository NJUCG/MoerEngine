#ifndef RASTER_LIGHTING_IBL_HLSLI
#define RASTER_LIGHTING_IBL_HLSLI

#include "core/common/Bindless.hlsl"
#include "shared/raster/ShaderParameters.h"

float3 calculate_ibl(Moer::LightingData lighting_data, float3 world_pos, uint cubemap_handle) {
    float3 view_dir = normalize(world_pos - lighting_data.camera_position);
    return TextureHandle(cubemap_handle).SampleCube<float3>(view_dir);
}

#endif