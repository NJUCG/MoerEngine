#ifndef RASTER_LIGHTING_IBL_HLSLI
#define RASTER_LIGHTING_IBL_HLSLI

#include "core/common/Bindless.hlsl"
#include "pipelines/raytracing/lighting/common/Lighting.hlsl"
#include "shared/raster/ShaderParameters.h"

float4 calculate_ibl(Moer::LightingData lighting_data, float3 world_pos,uint skybox_handles[6]) {
    float3 view_dir = world_pos - lighting_data.camera_position;

    float3 abs_dir = abs(view_dir);
    uint   axis    = 0;
    float2 uv;
    uint   handle_index;

    if (abs_dir.x >= abs_dir.y && abs_dir.x >= abs_dir.z) {
        axis = 0;
    } else if (abs_dir.y >= abs_dir.z) {
        axis = 1;
    } else {
        axis = 2;
    }

    if (axis == 0) {
        if (view_dir.x > 0) {
            uv = float2(-view_dir.z, -view_dir.y) / view_dir.x * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[2]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(view_dir.z, -view_dir.y) / (-view_dir.x) * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[3]).Sample2D<float3>(uv), 1.0);
        }
    } else if (axis == 1) {
        if (view_dir.y > 0) {
            uv = float2(view_dir.x, view_dir.z) / view_dir.y * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[4]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(view_dir.x, -view_dir.z) / (-view_dir.y) * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[5]).Sample2D<float3>(uv), 1.0);
        }
    } else {
        if (view_dir.z > 0) {
            uv = float2(view_dir.x, -view_dir.y) / view_dir.z * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[0]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(-view_dir.x, -view_dir.y) / (-view_dir.z) * 0.5 + 0.5;
            return float4(TextureHandle(skybox_handles[1]).Sample2D<float3>(uv), 1.0);
        }
    }
}

#endif