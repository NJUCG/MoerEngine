/**
  * SSAO implementation
  * Reference: GAMES202
  */

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/raster/post_process/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::AoPipelineBindlessParam> param;

#define AO_MODE_NONE 0
#define AO_MODE_SSAO 1
#define AO_MODE_SSAO_AO_ONLY 2
#define AO_MODE_SSDO 3
#define AO_MODE_SSDO_AO_ONLY 4

static const float Epsilon = 0.0001; // same with PBRMaterialFrag.hlsl
static const float3 ABNORMAL_COLOR = float3(0.0, 0.0, 1.0);

// uv in [0, 1]; output in [0, 1]
float2 random_2to2(float2 uv) {
    return TextureHandle(param.noise_tex).Sample2D<float4>(uv).rg;
}

// reference: games202 & https://www.shadertoy.com/view/Ms33WB
float ssao_games202(float2 uv) {
    float3 normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 position = TextureHandle(param.position_tex).Sample2D<float3>(uv);

    // if (uv.x < param.inv_resolution.x && uv.y < param.inv_resolution.y) {
    //     printf("uv: %f, %f; pos: %f, %f, %f\n", uv.x, uv.y, position.x, position.y, position.z);
    // }

    float ao = 0.0;
    float2 tmp1 = param.ssao_radius * param.inv_resolution;

    for (uint i = 0; i < param.ssao_sample_count; i++) {
        float2 offset = random_2to2(uv + 0.093 * float2(i, i)) * 2.0 - 1.0;
        float3 sample_position = TextureHandle(param.position_tex).Sample2D<float4>(uv + offset * tmp1).rgb;

        float3 vec = sample_position - position;
        float3 len = length(vec);
        float3 norm_vec = vec / len;

        ao += max(0.0, dot(normal, norm_vec) - 0.05) * smoothstep(param.ssao_max_distance, param.ssao_max_distance * 0.5, len);
    }
    ao = clamp(1.0 - ao / param.ssao_sample_count * param.ssao_intensity, 0.0, 1.0);

    return ao;
}

float4 main(float4 _pos: SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {

    float3 color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;

    if (param.ao_mode == AO_MODE_NONE) {
        return float4(color, 1.0);

    } else if (param.ao_mode == AO_MODE_SSAO) {
        float3 ssao_result = ssao_games202(uv);
        return float4(ssao_result * color, 1.0);

    } else if (param.ao_mode == AO_MODE_SSAO_AO_ONLY) {
        float3 ssao_result = ssao_games202(uv);
        return float4(ssao_result, 1.0);

    } else {
        return float4(ABNORMAL_COLOR, 1.0);
    }
}