#ifndef RASTER_LIGHTING_SHADOWS_HLSLI
#define RASTER_LIGHTING_SHADOWS_HLSLI

#include "core/common/Common.hlsl"
#include "core/common/Bindless.hlsl"
#include "pipelines/raytracing/lighting/common/Lighting.hlsl" // 需要 LightingData 定义
#include "shared/raster/ShaderParameters.h"


static const float2 POISSON_DISK_16[16] = {
    float2( -0.94201624, -0.39906216 ), float2(  0.94558609, -0.76890725 ),
    float2( -0.09418410, -0.92938870 ), float2(  0.34495938,  0.29387760 ),
    float2( -0.91588581,  0.45771432 ), float2( -0.81544232, -0.87912464 ),
    float2( -0.38277543,  0.27676845 ), float2(  0.97484398,  0.75648379 ),
    float2(  0.44323325, -0.97511554 ), float2(  0.53742981, -0.47373420 ),
    float2( -0.26496911, -0.41893023 ), float2(  0.79197514,  0.19090188 ),
    float2( -0.24188840,  0.99706507 ), float2( -0.81409955,  0.91437590 ),
    float2(  0.19984126,  0.78641367 ), float2(  0.14383161, -0.14100790 )
};

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
        if (lighting_data.shadow_csm_visualize_cascade != 0) {
            return visualize_csm_cascade(lighting_data, world_pos);
        }
        return calculate_csm(lighting_data, world_pos);
    }
    return 1.0;
}

#endif