#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "core/common/Common.hlsl"
#include "pipelines/raytracing/lighting/common/Lighting.hlsl"
#include "materials/Material.hlsl"

#include "shared/raster/ShaderParameters.h"

static const float2 POISSON_DISK_16[16] = {
    float2( -0.94201624, -0.39906216 ),
    float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ),
    float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ),
    float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ),
    float2(  0.97484398,  0.75648379 ),
    float2(  0.44323325, -0.97511554 ),
    float2(  0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ),
    float2(  0.79197514,  0.19090188 ),
    float2( -0.24188840,  0.99706507 ),
    float2( -0.81409955,  0.91437590 ),
    float2(  0.19984126,  0.78641367 ),
    float2(  0.14383161, -0.14100790 )
};

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

// 获取Cascade Index
int get_cascade_index(Moer::LightingData lighting_data, float3 world_pos) {
    float pixel_view_pos_z = abs(mul(lighting_data.view_matrix, float4(world_pos, 1.0)).z);
    float pixel_depth_ratio =
        (pixel_view_pos_z - lighting_data.near_clip) / (lighting_data.far_clip - lighting_data.near_clip);
    for (int i = 0; i < lighting_data.shadow_csm_num_of_cascades; i++) {
        if (pixel_depth_ratio < lighting_data.cascade_split_ratios[i]) {
            return i;
        }
    }
    return -1;
}

float get_cascade_blend_ratio(Moer::LightingData lighting_data, float3 world_pos, int cascade_index) {
    float pixel_view_pos_z =
        abs(mul(lighting_data.view_matrix, float4(world_pos, 1.0)).z); //FIXME:需要取负吗�?
    float blend_band_start_z =
        lighting_data.near_clip + lighting_data.cascade_blend_start_ratios[cascade_index] *
                                      (lighting_data.far_clip - lighting_data.near_clip);
    float blend_band_end_z = lighting_data.near_clip + lighting_data.cascade_split_ratios[cascade_index] *
                                                           (lighting_data.far_clip - lighting_data.near_clip);
    return smoothstep(blend_band_start_z, blend_band_end_z, pixel_view_pos_z);
}

float get_blocker_depth(Moer::LightingData lighting_data,float2 uv,float fragment_depth,int cascade_index)
{
    float search_radius_uv = lighting_data.light_size_world / (lighting_data.shadow_csm_sm_size * fragment_depth);
    uint num_blockers=0;
    float avg_blocker_depth=0.0;
    
    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i] * search_radius_uv;
        float occluder_depth = TextureHandle(lighting_data.shadow_map[cascade_index]).Sample2D<float>(uv + offset).x;

        if (occluder_depth > fragment_depth + SHADOW_BIAS) {
            avg_blocker_depth += occluder_depth;
            num_blockers++;
        }
    }

    if (num_blockers == 0) {
        return -1.0; // special value indicating no blockers found
    }

    return avg_blocker_depth / (float)num_blockers;
}

float calculate_penumbra_size(
    Moer::LightingData lighting_data,
    float fragment_depth,
    float avg_blocker_depth,
    float shadow_clip_w
) {
    if (avg_blocker_depth < 0.0) {
        return 0.0;
    }

    float penumbra_radius_ndc = 
        max(avg_blocker_depth-fragment_depth, 0.0) / (1.0-avg_blocker_depth + 1e-6) * lighting_data.light_size_world;

    float penumbra_radius_uv = penumbra_radius_ndc / (lighting_data.shadow_csm_sm_size * shadow_clip_w);

    return clamp(penumbra_radius_uv, 0.0, 0.1); // empirical maximum value to prevent excessive blur
}

float get_pcf_filter_result(Moer::LightingData lighting_data,float2 uv,float fragment_depth,float pcf_radius_uv,uint cascade_index)
{
    if(pcf_radius_uv<=0.0)return 1.0;//没有半影

    float shadow_contribution = 0.0;

    for (int i = 0; i < 16; ++i) {
        float2 offset = POISSON_DISK_16[i] * pcf_radius_uv;
        float occluder_depth = TextureHandle(lighting_data.shadow_map[cascade_index]).Sample2D<float>(uv + offset).x;
        
        if (occluder_depth > fragment_depth + SHADOW_BIAS) {
            shadow_contribution += 1.0;
        }
    }
    
    return 1.0 - (shadow_contribution / 16);
}

float get_single_shadow(Moer::LightingData lighting_data, float3 world_pos, int cascade_index) {
    float4 shadow_clip_pos = mul(lighting_data.world_to_shadow_clip[cascade_index], float4(world_pos, 1.0));
    float3 shadow_ndc_pos  = shadow_clip_pos.xyz / shadow_clip_pos.w;
    float2 shadow_uv       = float2(shadow_ndc_pos.x * 0.5 + 0.5, 1.0 - (shadow_ndc_pos.y * 0.5 + 0.5));
    if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
        shadow_ndc_pos.z >= 0.0 && shadow_ndc_pos.z <= 1.0) {
        float occluder_depth =
            TextureHandle(lighting_data.shadow_map[cascade_index]).Sample2D<float>(shadow_uv).x;
        float fragment_depth = shadow_ndc_pos.z;
        if(lighting_data.pcss_enabled==1)
        {
            float avg_blocker_depth = get_blocker_depth(lighting_data, shadow_uv, fragment_depth, cascade_index);
            //return avg_blocker_depth;
            float penumbra_size = calculate_penumbra_size(
                lighting_data,
                fragment_depth,
                avg_blocker_depth,
                shadow_clip_pos.w
            );
            float pcf_result = get_pcf_filter_result(lighting_data,shadow_uv, fragment_depth, penumbra_size, cascade_index);
            return pcf_result;
        }
        else{
            return (fragment_depth + SHADOW_BIAS < occluder_depth) ? 0.0 : 1.0;
        }
        // near=1.0, reverse-z
    }
    return 1.0;
}

float calculate_csm(Moer::LightingData lighting_data, float3 world_pos) {
    int cascade_index = get_cascade_index(lighting_data, world_pos);
    if (cascade_index == -1)
        return 1.0;

    if (lighting_data.is_csm_blend_enabled == 1) {
        float shadow_current = get_single_shadow(lighting_data, world_pos, cascade_index);
        float shadow_next    = (cascade_index + 1 < lighting_data.shadow_csm_num_of_cascades) ?
                                   get_single_shadow(lighting_data, world_pos, cascade_index + 1) :
                                   1.0;

        float cascade_blend_ratio = get_cascade_blend_ratio(lighting_data, world_pos, cascade_index);
        return lerp(shadow_current, shadow_next, cascade_blend_ratio);
    } else {
        return get_single_shadow(lighting_data, world_pos, cascade_index);
    }
}

float visualize_csm_cascade(Moer::LightingData lighting_data, float3 world_pos) {
    int idx = get_cascade_index(lighting_data, world_pos);
    return 1.0 * idx / lighting_data.shadow_csm_num_of_cascades;
}

float calculate_shadow(Moer::LightingData lighting_data, float3 world_pos) {
    if (lighting_data.shadow_map_mode == Moer::EShadowMapMode::NONE) {
        return 1.0;

    } else if (lighting_data.shadow_map_mode == Moer::EShadowMapMode::CSM ||
               lighting_data.shadow_map_mode == Moer::EShadowMapMode::CSM_AUTO) {
        // 用黑白来可视化CSM层数。全黑表�?层，全白表示最大层
        if (lighting_data.shadow_csm_visualize_cascade != 0) {
            return visualize_csm_cascade(lighting_data, world_pos);
        }
        // 正常渲染shadow
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
        axis = 0; // X�?
    } else if (abs_dir.y >= abs_dir.z) {
        axis = 1; // Y�?
    } else {
        axis = 2; // Z�?
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