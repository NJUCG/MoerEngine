#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/post_process/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::SsdoPipelineBindlessParam> param;

#include "AoCommon.hlsl"

static const float3 ABNORMAL_COLOR = float3(0.0, 0.0, 1.0);

// uv in [0, 1]; output in [0, 1]
// float2 random_2to2(float2 uv) {
//     return TextureHandle(param.noise_tex).Sample2D<float4>(uv).rg;
// }

float random_1to1(float2 seed) {
    // 使用 sin 和一个大数的小数部分来产生伪随机性
    return frac(sin(dot(seed, float2(12.9898, 78.233))) * 43758.5453123);
}

float2 random_2to2(float2 seed) {
    return float2(
        frac(sin(dot(seed, float2(12.9898, 78.233))) * 43758.5453123),
        frac(sin(dot(seed, float2(45.123, 98.456))) * 43758.5453123)
    );
}

//返回一个随机的半球方向，符合余弦加权分布，但向量长度永远为1
float3 SampleHemisphere_Cosine(float3 N, float2 randomValues) {
    float phi      = 2.0 * PI * randomValues.x;
    float cosTheta = sqrt(1.0 - randomValues.y);
    float sinTheta = sqrt(randomValues.y);

    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3   up        = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3   tangent   = normalize(cross(up, N));
    float3   bitangent = cross(N, tangent);
    float3x3 TBN       = float3x3(tangent, bitangent, N);

    return mul(TBN, H);
}

float3 apply_view_projection(float3 position) {
    float4 p = mul(param.view_projection_matrix, float4(position, 1.0));
    p /= p.w;
    return float3(p.x * 0.5 + 0.5, -p.y * 0.5 + 0.5, p.z);
}

bool isUvdValid(float3 uv) {
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && uv.z >= 0.0 && uv.z <= 1.0;
}

float GetDepthFromWorldPos(float3 worldPos) {
    float4 clipPos = mul(param.view_projection_matrix, float4(worldPos, 1.0));
    return clipPos.z / clipPos.w;
}

float3 GetVplIndirectLight(
    float3 vpl_pos,
    float3 vpl_normal,
    float3 shading_pos,
    float3 shading_normal,
    float3 pixel_color
) {
    float3 light_dir     = normalize(vpl_pos - shading_pos);
    float  shadingCosine = max(dot(shading_normal, light_dir), 0.0);
    float  vplCosine     = max(dot(vpl_normal, -light_dir), 0.0);
    float  VPL_distance  = length(vpl_pos - shading_pos);
    float  attenuation   = 1.0 / (VPL_distance * VPL_distance + 0.001); // 避免除零

    // 简单的漫反射间接光
    float3 indirect_light = shadingCosine * vplCosine * attenuation * pixel_color;

    return indirect_light;
}

float3 GetVplContribution(float2 vpl_uv, float2 shading_uv) {
    float3 vpl_pos = TextureHandle(param.position_tex).Sample2D<float3>(vpl_uv);
    float3 vpl_normal =
        normalize(Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(vpl_uv)));
    float3 shading_pos = TextureHandle(param.position_tex).Sample2D<float3>(shading_uv);
    float3 shading_normal =
        normalize(Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(shading_uv)));
    float3 pixel_color = TextureHandle(param.input_image).Sample2D<float3>(vpl_uv);
    return GetVplIndirectLight(vpl_pos, vpl_normal, shading_pos, shading_normal, pixel_color);
}

float4 GetSsdo(float2 uv) {
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 position = TextureHandle(param.position_tex).Sample2D<float3>(uv);

    float  occlusion           = 0.0;
    float3 indirect_light      = float3(0.0, 0.0, 0.0);
    float  ssdo_sample_count_f = (float)param.ssdo_sample_count;

    for (uint i = 0; i < param.ssdo_sample_count; i++) {
        float2 xi                   = random_2to2(uv + float(i) * 0.1);
        float3 hemisphereSampleVec3 = SampleHemisphere_Cosine(normal, xi);

        // 使用抖动的分层采样来计算采样距离
        float sampleScale = ((float)i + random_1to1(uv - float(i))) / ssdo_sample_count_f;
        sampleScale       = lerp(0.1, 1.0, sampleScale * sampleScale);

        float3 sampleWorldPos = position + hemisphereSampleVec3 * param.ssdo_radius * xi.x;

        float3 sampleUVD = apply_view_projection(sampleWorldPos);

        if (!isUvdValid(sampleUVD)) {
            continue;
        }

        float  sceneDepthOfUV = TextureHandle(param.depth_tex).Sample2D<float>(sampleUVD.xy);
        float3 scenePosOfUV   = TextureHandle(param.position_tex).Sample2D<float4>(sampleUVD.xy).rgb;
        float  distFromVplToShadingPoint = length(scenePosOfUV - position);

        if ((sampleUVD.z + param.ssdo_depth_bias) < sceneDepthOfUV) {
            float falloff =
                smoothstep(param.ssdo_max_distance, param.ssdo_max_distance * 0.5, distFromVplToShadingPoint);
            occlusion += falloff;
            indirect_light += GetVplContribution(sampleUVD.xy, uv);
        }
    }

    if (param.ssdo_sample_count > 0) {
        occlusion /= ssdo_sample_count_f;
        indirect_light /= ssdo_sample_count_f;
    }

    return float4(
        indirect_light * param.ssdo_indirect_intensity,
        clamp(1.0 - occlusion * param.ssdo_intensity, 0.0, 1.0)
    );
}

AoOutput main(float2 uv : TEXCOORD0) {
    AoOutput output;
    float3   color = TextureHandle(param.input_image).Sample2D<float3>(uv);

    output.camera_motion_vector = GetCameraMotionVector(uv);

    output.camera_motion_vector = output.camera_motion_vector * 0.5f + output.color_with_ao.xy * 0.5f;

    float4 ssdo_result = GetSsdo(uv);

    if (param.ao_mode == Moer::EAoMode::NONE) {
        output.color_with_ao = float4(color, 1.0);
        output.ambient_only  = 1.0;
    } else if (param.ao_mode == Moer::EAoMode::SSDO) {
        output.ambient_only  = ssdo_result.w;
        output.color_with_ao = float4(ssdo_result.xyz, 1.0) + float4(ssdo_result.w * color, 1.0);
    } else if (param.ao_mode == Moer::EAoMode::SSDO_AO_ONLY) {
        output.ambient_only  = ssdo_result.w;
        output.color_with_ao = float4(ssdo_result.w, ssdo_result.w, ssdo_result.w, 1.0);
    }

    return output;
}

//===================magic ssao-based ssdo code===================

// float3 get_ssdo_ao_test(float2 uv) {
//     float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
//     float3 position = TextureHandle(param.position_tex).Sample2D<float3>(uv);

//     float  ao   = 0.0;
//     float2 tmp1 = param.ssdo_radius * param.inv_resolution;

//     for (uint i = 0; i < param.ssdo_sample_count; i++) {
//         float2 offset          = random_2to2(uv + 0.093 * float2(i, i)) * 2.0 - 1.0;
//         float3 sample_position = TextureHandle(param.position_tex).Sample2D<float4>(uv + offset * tmp1).rgb;

//         float3 vec      = sample_position - position;
//         float3 len      = length(vec);
//         float3 norm_vec = vec / len;

//         ao += max(0.0, dot(normal, norm_vec) - 0.05) *
//               smoothstep(param.ssdo_max_distance, param.ssdo_max_distance * 0.5, len);
//     }
//     ao = clamp(1.0 - ao / param.ssdo_sample_count * param.ssdo_intensity, 0.0, 1.0);

//     return ao;
// }

// float3 get_ssdo_indirect_test(float2 uv) {
//     float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
//     float3 position = TextureHandle(param.position_tex).Sample2D<float3>(uv);

//     float3 indirect_light = float3(0.0, 0.0, 0.0);
//     float2 tmp1           = param.ssdo_radius * param.inv_resolution;

//     for (uint i = 0; i < param.ssdo_sample_count; i++) {
//         float2 offset          = random_2to2(uv + 0.093 * float2(i, i)) * 2.0 - 1.0;
//         float3 sample_position = TextureHandle(param.position_tex).Sample2D<float4>(uv + offset * tmp1).rgb;

//         float3 vec      = sample_position - position;
//         float3 len      = length(vec);
//         float3 norm_vec = vec / len;

//         float ao_to_add = max(0.0, dot(normal, norm_vec) - 0.05) *
//                           smoothstep(param.ssdo_max_distance, param.ssdo_max_distance * 0.5, len);
//         indirect_light += ao_to_add * GetVplContribution(uv + offset * tmp1, uv);
//     }

//     return indirect_light / (float)param.ssdo_sample_count * 0.01;
// }

// AoOutput main(float2 uv : TEXCOORD0) {
//     AoOutput output;
//     float3   color = TextureHandle(param.input_image).Sample2D<float3>(uv);

//     output.camera_motion_vector = GetCameraMotionVector(uv);

//     output.camera_motion_vector = output.camera_motion_vector * 0.5f + output.color_with_ao.xy * 0.5f;

//     float ao = get_ssdo_ao_test(uv);

//     if (param.ao_mode == Moer::EAoMode::NONE) {
//         output.color_with_ao = float4(color, 1.0);
//         output.ambient_only  = 1.0;
//     } else if (param.ao_mode == Moer::EAoMode::SSDO) {
//         output.ambient_only  = ao;
//         output.color_with_ao = float4(get_ssdo_indirect_test(uv) * ao, 1.0) + float4(ao * color, 1.0);
//     } else if (param.ao_mode == Moer::EAoMode::SSDO_AO_ONLY) {
//         output.ambient_only  = ao;
//         output.color_with_ao = float4(ao, ao, ao, 1.0);
//     }

//     return output;
// }