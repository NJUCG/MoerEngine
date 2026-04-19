#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3)

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::SsdoPipelineBindlessParam> param;

#include "pipelines/postprocess/lighting_effects/AoCommon.hlsl"

static const float3 DIFFUSE_ALBEDO = float3(0.5, 0.5, 0.5);

float random_1to1(float2 seed) {
    // 使用 sin 和一个大数的小数部分来产生伪随机�?
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
    float4 p = mul(param.world2clip, float4(position, 1.0));
    p /= p.w;
    return float3(p.x * 0.5 + 0.5, -p.y * 0.5 + 0.5, p.z);
}

bool isUvdValid(float3 uv) {
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && uv.z >= 0.0 && uv.z <= 1.0;
}

float GetDepthFromWorldPos(float3 worldPos) {
    float4 clipPos = mul(param.world2clip, float4(worldPos, 1.0));
    return clipPos.z / clipPos.w;
}

float3 GetVplIndirectLight(float3 vpl_pos, float3 vpl_normal, float3 shading_pos, float3 pixel_color) {
    float3 light_dir    = normalize(vpl_pos - shading_pos);
    float  vplCosine    = max(dot(vpl_normal, -light_dir), 0.0);
    float  VPL_distance = length(vpl_pos - shading_pos);
    float  attenuation  = 1.0 / (VPL_distance * VPL_distance + 1.0); // 稳定衰减

    // 面积�?
    // vpl_linear_depth ≈ abs(view_z), 透视投影下 clip.w == view_z
    float4 vpl_clip         = mul(param.world2clip, float4(vpl_pos, 1.0));
    float  vpl_linear_depth = abs(vpl_clip.w);
    float  area_weight      = vpl_linear_depth * vpl_linear_depth + 0.0001;
    area_weight             = min(2, area_weight); //防止过大

    // 简单的漫反射间接光
    //由于采用了余弦加权采样，这里不用再乘�?shadingCosine �?
    float3 indirect_light = vplCosine * pixel_color * attenuation * area_weight;

    return indirect_light;
}

float3 GetVplContribution(float2 vpl_uv, float2 shading_uv) {
    float3 vpl_pos = WorldPosFromDepthTexture(param.depth_tex, vpl_uv, param.clip2world);
    float3 vpl_normal =
        normalize(Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(vpl_uv)));
    float3 shading_pos = WorldPosFromDepthTexture(param.depth_tex, shading_uv, param.clip2world);
    float3 pixel_color = TextureHandle(param.input_image).Sample2D<float3>(vpl_uv);
    return GetVplIndirectLight(vpl_pos, vpl_normal, shading_pos, pixel_color);
}

float4 GetSsdo(float2 uv) {
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
    float3 position = WorldPosFromDepthTexture(param.depth_tex, uv, param.clip2world);

    float  occlusion           = 0.0;
    float3 indirect_light      = float3(0.0, 0.0, 0.0);
    float  ssdo_sample_count_f = (float)param.ssdo_sample_count;

    for (uint i = 0; i < param.ssdo_sample_count; i++) {
        float2 xi                   = random_2to2(uv + float(i) * 0.1);
        float3 hemisphereSampleVec3 = SampleHemisphere_Cosine(normal, xi);

        // 使用抖动的分层采样来计算采样距离
        float sampleScale = ((float)i + random_1to1(uv - float(i))) / ssdo_sample_count_f;
        sampleScale       = lerp(0.01, 1.0, sampleScale * sampleScale);

        float3 sampleWorldPos = position + hemisphereSampleVec3 * param.ssdo_radius * sampleScale;

        float3 sampleUVD = apply_view_projection(sampleWorldPos);

        if (!isUvdValid(sampleUVD)) {
            continue;
        }

        float  sceneDepthOfUV = TextureHandle(param.depth_tex).Sample2D<float>(sampleUVD.xy);
        float3 scenePosOfUV   = WorldPosFromDepthTexture(param.depth_tex, sampleUVD.xy, param.clip2world);
        float  distFromVplToShadingPoint = length(scenePosOfUV - position);

        if ((sampleUVD.z + param.ssdo_depth_bias) < sceneDepthOfUV) { //reversed z!
            float falloff =
                smoothstep(param.ssdo_max_distance, param.ssdo_max_distance * 0.5, distFromVplToShadingPoint);
            occlusion += falloff;
            //indirect_light += GetVplContribution(sampleUVD.xy, uv);
        } else {
            indirect_light += GetVplContribution(sampleUVD.xy, uv);
        }
    }

    if (param.ssdo_sample_count > 0) {
        occlusion /= ssdo_sample_count_f;
        indirect_light /= ssdo_sample_count_f;
        indirect_light *= DIFFUSE_ALBEDO;
    }

    return float4(
        indirect_light * param.ssdo_indirect_intensity,
        clamp(1.0 - occlusion * param.ssdo_intensity, 0.0, 1.0)
    );
}

AoOutput main(float2 uv : TEXCOORD0) {
    AoOutput output;

    output.camera_motion_vector = GetCameraMotionVector(uv);

    // FIXME: 目前AO输出结果只有AO值，SSDO坏掉了，需要重构
    float4 ssdo_result  = GetSsdo(uv);
    output.ambient_only = ssdo_result.w;

    return output;
}
