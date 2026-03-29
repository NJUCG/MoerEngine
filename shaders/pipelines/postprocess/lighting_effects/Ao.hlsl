/**
  * SSAO implementation
  * Reference: GAMES202
  */

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/RasterCommon.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::AoPipelineBindlessParam> param;

// 定义了AoOutput、CameraMotionVector等函�?
#include "pipelines/postprocess/lighting_effects/AoCommon.hlsl"

static const float3 ABNORMAL_COLOR = float3(0.0, 0.0, 1.0);

// uv in [0, 1]; output in [0, 1]
float2 random_2to2(float2 uv) {
    return TextureHandle(param.noise_tex).Sample2D<float4>(uv).rg;
}

// reference: games202 & https://www.shadertoy.com/view/Ms33WB
float ssao_games202(float2 uv) {
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 position = WorldPosFromDepthTexture(param.depth_tex, uv, param.clip2world);

    // if (uv.x < param.inv_resolution.x && uv.y < param.inv_resolution.y) {
    //     printf("uv: %f, %f; pos: %f, %f, %f\n", uv.x, uv.y, position.x, position.y, position.z);
    // }

    float  ao   = 0.0;
    float2 tmp1 = param.ssao_radius * param.inv_resolution;

    for (uint i = 0; i < param.ssao_sample_count; i++) {
        float2 offset          = random_2to2(uv + 0.093 * float2(i, i)) * 2.0 - 1.0;
        float3 sample_position = WorldPosFromDepthTexture(param.depth_tex, uv + offset * tmp1, param.clip2world);

        float3 vec      = sample_position - position;
        float3 len      = length(vec);
        float3 norm_vec = vec / len;

        ao += max(0.0, dot(normal, norm_vec) - 0.05) * smoothstep(param.ssao_max_distance, param.ssao_max_distance * 0.5, len);
    }
    ao = clamp(1.0 - ao / param.ssao_sample_count * param.ssao_intensity, 0.0, 1.0);

    return ao;
}

float get_ao(float2 uv) {
    return ssao_games202(uv);
}

AoOutput main(float2 uv : TEXCOORD0) {
    AoOutput output;
    
    float3 color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;


    if (param.ao_mode == Moer::EAoMode::NONE) {
        output.color_with_ao = float4(color, 1.0);
        output.ambient_only  = 1.0;

    } else if (param.ao_mode == Moer::EAoMode::SSAO) {
        float ao = get_ao(uv);

        output.color_with_ao = float4(color * ao, 1.0);
        output.ambient_only  = ao;

    } else if (param.ao_mode == Moer::EAoMode::SSAO_AO_ONLY) {
        float ao = get_ao(uv);

        output.color_with_ao = float4(ao, ao, ao, 1.0);
        output.ambient_only  = ao;

    } else {
        output.color_with_ao = float4(ABNORMAL_COLOR, 1.0);
        output.ambient_only  = 1.0;
    }

    output.camera_motion_vector = GetCameraMotionVector(uv);

    output.camera_motion_vector = output.camera_motion_vector * 0.5f + output.color_with_ao.xy * 0.5f;

    return output;
}