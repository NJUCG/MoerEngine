#include "framework/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "framework/Common.hlsl"
#include "framework/Lighting.hlsl"
#include "framework/Material.hlsl"

#include "shared/raster/lighting_pass/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::MaterialPassBindlessParam> param;

float ndfGGX(float cosLh, float roughness) {
    float alpha   = roughness * roughness;
    float alphaSq = alpha * alpha;
    float denom   = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * denom * denom);
}

float gaSchlickG1(float cosTheta, float k) {
    return cosTheta / (cosTheta * (1.0 - k) + k);
}

float gaSchlickGGX(float cosLi, float cosLo, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return gaSchlickG1(cosLi, k) * gaSchlickG1(cosLo, k);
}

float3 fresnelSchlick(float3 F0, float cosTheta) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

static const float3 Fdielectric = 0.04;
static const float  Epsilon     = 0.0001;
struct PBRInfo {
    float  roughness;
    float3 albedo;
    float  metalness;
    float3 normal;
    float3 viewDir;

    float3 Evaluate(float3 lightDir) {
        float3 F0           = lerp(Fdielectric, albedo, metalness);
        float3 halfDir      = normalize(lightDir + viewDir);
        float  cosLi        = saturate(dot(normal, lightDir));
        float  cosLh        = saturate(dot(normal, halfDir));
        float  cosLo        = saturate(dot(normal, viewDir));
        float3 F            = fresnelSchlick(F0, cosLo);
        float  D            = ndfGGX(cosLh, roughness);
        float  G            = gaSchlickGGX(cosLi, cosLo, roughness);
        float3 kd           = lerp(float3(1, 1, 1) - F, float3(0, 0, 0), metalness);
        float3 diffuseBRDF  = kd * albedo;
        float3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);
        return (diffuseBRDF + specularBRDF) * cosLi;
    }
};

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 inv_view_proj) {
    float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
    float4 world_w = mul(inv_view_proj, clip);
    float3 pos     = world_w.xyz / world_w.w;
    return pos;
}

int get_cascade_index(Moer::LightingData lighting_data, float3 world_pos) {
    //FIXME:移动到cpu端计算，会提高效率么？
    float4 pixel_view_pos    = mul(param.view_matrix, float4(world_pos, 1.0));
    float  pixel_depth_ratio = (pixel_view_pos.z - param.near_clip) / (param.far_clip - param.near_clip);
    for (int i = 0; i < lighting_data.shadow_csm_num_of_cascades; i++) {
        if (pixel_depth_ratio < param.csm_split_ratios[i]) {
            return i;
        }
    }
    return -1;
}

float calculate_csm(Moer::LightingData lighting_data, float3 world_pos) {
    int cascade_index = get_cascade_index(lighting_data, world_pos);
    if (cascade_index == -1)
        return 1.0;
    float4 shadow_clip_pos = mul(lighting_data.world_to_shadow_clip[cascade_index], float4(world_pos, 1.0));
    float3 shadow_ndc_pos  = shadow_clip_pos.xyz / shadow_clip_pos.w;
    float2 shadow_uv       = float2(shadow_ndc_pos.x * 0.5 + 0.5, 1.0 - (shadow_ndc_pos.y * 0.5 + 0.5));
    if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
        shadow_ndc_pos.z >= 0.0 && shadow_ndc_pos.z <= 1.0) {
        float occluder_depth =
            TextureHandle(lighting_data.shadow_map[cascade_index]).Sample2D<float>(shadow_uv).x;
        float fragment_depth = shadow_ndc_pos.z;
        return (shadow_ndc_pos.z + SHADOW_BIAS < occluder_depth) ? 0.0 : 1.0;
        // near<->1.0; far<->0.0
    }
    return 1.0;
}

float calculate_shadow(Moer::LightingData lighting_data, float3 world_pos) {
    if (lighting_data.shadow_map_mode == 0) {
        return 1.0;
    } else if (lighting_data.shadow_map_mode == 1) {
        return calculate_csm(lighting_data, world_pos);
    } else if (lighting_data.shadow_map_mode == 2) {
        return calculate_csm(lighting_data, world_pos);
    } else {
        return 1.0;
    }
}

float4 calculate_ibl(Moer::LightingData lighting_data, float3 world_pos) {
    float3 view_dir = world_pos - lighting_data.camera_position;

    float3 abs_dir = abs(view_dir);
    uint   axis    = 0;
    float2 uv;
    uint   handle_index;

    if (abs_dir.x >= abs_dir.y && abs_dir.x >= abs_dir.z) {
        axis = 0; // X轴
    } else if (abs_dir.y >= abs_dir.z) {
        axis = 1; // Y轴
    } else {
        axis = 2; // Z轴
    }

    if (axis == 0) {
        if (view_dir.x > 0) {
            uv = float2(-view_dir.z, -view_dir.y) / view_dir.x * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[2]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(view_dir.z, -view_dir.y) / (-view_dir.x) * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[3]).Sample2D<float3>(uv), 1.0);
        }
    } else if (axis == 1) {
        if (view_dir.y > 0) {
            uv = float2(view_dir.x, view_dir.z) / view_dir.y * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[4]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(view_dir.x, -view_dir.z) / (-view_dir.y) * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[5]).Sample2D<float3>(uv), 1.0);
        }
    } else {
        if (view_dir.z > 0) {
            uv = float2(view_dir.x, -view_dir.y) / view_dir.z * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[0]).Sample2D<float3>(uv), 1.0);
        } else {
            uv = float2(-view_dir.x, -view_dir.y) / (-view_dir.z) * 0.5 + 0.5;
            return float4(TextureHandle(param.skybox_handles[1]).Sample2D<float3>(uv), 1.0);
        }
    }
}

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: Textures
    uint gbuffer_mat = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
    uint mat_type    = gbuffer_mat & 0x000000FF;
    uint mat_id      = (gbuffer_mat & 0xFFFFFF00) >> 8;
    if (mat_type != param.material_type) {
        discard;
    }
    MaterialData mat = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);

    // MARK: Lighting Data
    ArrayBuffer        global_params = ArrayBuffer(param.global_param_handle);
    Moer::LightingData lighting_data = global_params.Load<Moer::LightingData>(0);

    // MARK: GBuffer
    float2 uv       = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    float  depth    = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);
    float3 normal   = Raster::UnpackNormal(TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv));
    float3 tangent  = Raster::UnpackNormal(TextureHandle(param.gbuffer_tangent).Sample2D<float3>(in_uv));
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.inv_view_proj);
    // Shoude be reconstructed from depth
    // Old code: float3 position = TextureHandle(param.gbuffer_position).Sample2D<float3>(in_uv);

    // MARK: Skybox
    if (depth == 0.0) {
        float3 pos_inf = WorldPosFromDepth(0.99, in_uv, lighting_data.inv_view_proj);
        // printf("pos_inf: %f, %f, %f\n", pos_inf.x, pos_inf.y, pos_inf.z);
        return calculate_ibl(lighting_data, pos_inf);
        //return float4(0.0, 0.0, 0.0, 1.0); // Black Skybox
    }

    // MARK: PBR
    PBRInfo pbrInfo;

    // - Albedo
    pbrInfo.albedo =
        GetTextureData<float3>(mat.albedo_map, uv, mat.base_color_factor.xyz, MISSING_TEXTURE_COLOR);

    // - Metallic & Roughness
    float2 metallic_roughness = GetTextureData<float2>(
        mat.metallic_roughness_map,
        uv,
        float2(mat.metallic_factor, mat.roughness_factor),
        float2(mat.metallic_factor, mat.roughness_factor)
    );
    pbrInfo.metalness = metallic_roughness.x;
    pbrInfo.roughness = metallic_roughness.y;

    // - Normal
    pbrInfo.normal = GetNormalFromNormalMap(mat.normal_map, uv, normal, tangent);

    // FIXME: sponze - normal_map == 66 => bug
    if (mat.normal_map == 66) { // wtf...
        pbrInfo.normal = normal;
    }
    // float3 normal_map_test = TextureHandle(mat.normal_map).Sample2D<float3>(uv);
    // if (in_uv.x < 0.00032 && in_uv.y < 0.00056) {
    //     printf("normal_map: %d\n", mat.normal_map);
    // }
    // return float4(normal_map_test, 1.0);

    // MARK: Shading
    float3 color = float3(0, 0, 0);

    // - View Dir
    pbrInfo.viewDir = normalize(lighting_data.camera_position - position.xyz);

    // - Lights
    ArrayBuffer light_buffer = ArrayBuffer(param.light_buffer);

    // - Shadow
    float shadow = calculate_shadow(lighting_data, position);

    // - Shading
    for (uint i = 0; i < lighting_data.light_count; i++) {
        LightData light = light_buffer.Load<LightData>(i);

        float3 light_dir = calculate_light_dir(light, position, normal);

        float3 brdf = pbrInfo.Evaluate(light_dir);

        color += apply_light(light, position, normal, brdf, shadow);
    }

    if (param.enable_extra_ambient) {
        color += param.extra_ambient_intensity * param.extra_ambient_color * pbrInfo.albedo;
    }

    // // 可视化ShadowMap
    // float3 shadow_map_value = TextureHandle(lighting_data.shadow_map_0).Sample2D<float>(in_uv).xxx;
    // color = 0.5 * color + 0.5 * shadow_map_value;

    return float4(color, 1.0);
}

/*
Debug Prints:
        // // uv: 0.000312, 0.000556
        // if (in_uv.x < 0.00032 && in_uv.y < 0.00056 && i <= 5) {

        //     float3 albedo = pbrInfo.albedo;
        //     float metalness = pbrInfo.metalness;
        //     float roughness = pbrInfo.roughness;
        //     float3 viewDir = pbrInfo.viewDir;
        //     float3 normal = pbrInfo.normal;
        //     float3 lightDir = light_dir;

        //     float3 F0 = lerp(Fdielectric, albedo, metalness);
        //     float3 halfDir = normalize(lightDir + viewDir);
        //     float cosLi = saturate(dot(normal, lightDir));
        //     float cosLh = saturate(dot(normal, halfDir));
        //     float cosLo = saturate(dot(normal, viewDir));
        //     float3 F = fresnelSchlick(F0, cosLo);
        //     float D = ndfGGX(cosLh, roughness);
        //     float G = gaSchlickGGX(cosLi, cosLo, roughness);
        //     float3 kd = lerp(float3(1, 1, 1) - F, float3(0, 0, 0), metalness);
        //     float3 diffuseBRDF = kd * albedo;
        //     float3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

        //     printf("\n");
        //     printf(
        //         "Light Count: %d/%d; albedo %f; metalness %f; roughness %f; viewDir %f %f %f; normal %f %f %f; lightDir %f %f %f\n",
        //         i, lighting_data.light_count,
        //         albedo.x, metalness, roughness,
        //         viewDir.x, viewDir.y, viewDir.z,
        //         normal.x, normal.y, normal.z,
        //         lightDir.x, lightDir.y, lightDir.z
        //     );
        //     printf(
        //         "F0 %f; halfDir %f; cosLi %f; cosLh %f; cosLo %f; D %f; G %f; kd %f; diffuseBRDF %f; specularBRDF %f\n",
        //         F0.x, halfDir.x, cosLi, cosLh, cosLo, D, G, kd.x, diffuseBRDF.x, specularBRDF.x
        //     );
        //     printf(
        //         "metallic_roughness_map: %d; albedo_map: %d; normal_map: %d; mat.roughness_factor: %f (%f %f);\n",
        //         mat.metallic_roughness_map,
        //         mat.albedo_map,
        //         mat.normal_map,
        //         mat.roughness_factor,
        //         metallic_roughness.x,
        //         metallic_roughness.y
        //     );
        // }
*/