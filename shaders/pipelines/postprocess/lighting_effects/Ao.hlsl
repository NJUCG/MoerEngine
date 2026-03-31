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

// Per-pixel hash: 以整数像素坐标为输入，保证相邻像素得到不同的随机值，
// 不依赖 noise texture，彻底避免 UV 采样粒度导致的 screen-fixed pattern。
float2 hash22(float2 pixel, uint sample_idx) {
    float2 p = pixel + float2(float(sample_idx) * 17.0, float(sample_idx) * 31.0);
    p        = frac(p * float2(443.8975, 397.2973));
    p += dot(p.xy, p.yx + 19.19);
    return frac(float2(p.x * p.y, p.x + p.y));
}

// reference: games202 & https://www.shadertoy.com/view/Ms33WB
float ssao_games202(float2 uv) {
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 position = WorldPosFromDepthTexture(param.depth_tex, uv, param.clip2world);

    float  ao    = 0.0;
    float2 tmp1  = param.ssao_radius * param.inv_resolution;
    float2 pixel = floor(uv / param.inv_resolution); // 整数像素坐标

    for (uint i = 0; i < param.ssao_sample_count; i++) {
        float2 offset = hash22(pixel, i) * 2.0 - 1.0;
        float3 sample_position =
            WorldPosFromDepthTexture(param.depth_tex, uv + offset * tmp1, param.clip2world);

        float3 vec      = sample_position - position;
        float3 len      = length(vec);
        float3 norm_vec = vec / len;

        ao += max(0.0, dot(normal, norm_vec) - 0.05) *
              smoothstep(param.ssao_max_distance, param.ssao_max_distance * 0.5, len);
    }
    ao = clamp(1.0 - ao / param.ssao_sample_count * param.ssao_intensity, 0.0, 1.0);

    return ao;
}

float get_ao(float2 uv) {
    return ssao_games202(uv);
}

AoOutput main(float2 uv : TEXCOORD0) {
    AoOutput output;

    if (param.ao_mode == Moer::EAoMode::SSAO || param.ao_mode == Moer::EAoMode::SSAO_AO_ONLY) {
        output.ambient_only = get_ao(uv);
    } else {
        output.ambient_only = 1.0;
    }

    output.camera_motion_vector = GetCameraMotionVector(uv);
    return output;
}