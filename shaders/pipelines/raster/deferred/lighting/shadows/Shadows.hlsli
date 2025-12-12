#ifndef MOER_LIGHTING_SHADOWS_ENTRY_HLSLI
#define MOER_LIGHTING_SHADOWS_ENTRY_HLSLI

#include "pipelines/raster/deferred/lighting/shadows/CSM.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/PCSS.hlsli"

float get_single_shadow(
    Moer::LightingData lighting_data,
    float3             world_pos,
    int                cascade_index,
    float2             screen_uv,
    float3             normal,
    float3             lightDir
) {
    float4 shadow_clip_pos = mul(lighting_data.world_to_shadow_clip[cascade_index], float4(world_pos, 1.0));
    float3 shadow_ndc_pos  = shadow_clip_pos.xyz / shadow_clip_pos.w;
    float2 shadow_uv       = float2(shadow_ndc_pos.x * 0.5 + 0.5, 1.0 - (shadow_ndc_pos.y * 0.5 + 0.5));
    bool   in_bounds = shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
                     shadow_ndc_pos.z >= 0.0 && shadow_ndc_pos.z <= 1.0;
    if (!in_bounds) {
        return 1.0;
    }

    ShadowContext ctx;
    ctx.shadowMapHandle = lighting_data.cascade_shadow_map[cascade_index];
    ctx.shadowUV        = shadow_uv;
    ctx.fragmentDepth   = shadow_ndc_pos.z;
    ctx.screenUV        = screen_uv;
    ctx.lightSizeWorld  = lighting_data.light_size_world;
    ctx.shadowMapSize   = lighting_data.shadow_csm_sm_size;
    ctx.clipW           = shadow_clip_pos.w;
    ctx.scaleData       = lighting_data.scale_data[cascade_index];
    ctx.normal          = normal;
    ctx.lightDir        = lightDir;

    float occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(shadow_uv).x;
    float fragment_depth = shadow_ndc_pos.z;

    if (lighting_data.pcss_enabled == 1) {
        return calculate_pcss(ctx);
    } else {
        occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(ctx.shadowUV).x;
        return is_shadowed(occluder_depth, fragment_depth, SHADOW_BIAS) ? 0.0 : 1.0;
    }
}

float calculate_point_shadow(
    Moer::LightingData lighting_data,
    float3             world_pos,
    float2             screen_uv,
    float3             normal
) {
    float3 light_pos          = lighting_data.light_pos;
    float3 light_to_frag      = world_pos - light_pos;
    float  distance           = length(light_to_frag);
    uint   shadow_cube_handle = lighting_data.point_shadow_map;

    if (distance >= lighting_data.light_radius)
        return 1.0;

    float3 dir = normalize(light_to_frag);

    float occluder_depth = TextureHandle(shadow_cube_handle).SampleCube<float>(dir).r;

    float3 abs_vec = abs(light_to_frag);

    float local_z = max(abs_vec.x, max(abs_vec.y, abs_vec.z));

    float near_plane = lighting_data.near_clip;
    float far_plane  = lighting_data.far_clip;

    float fragment_depth = (near_plane / (near_plane - far_plane)) -
                           (near_plane * far_plane / (near_plane - far_plane)) / local_z;

    ShadowContext ctx;
    ctx.shadowMapHandle = shadow_cube_handle;
    ctx.fragmentDepth   = distance;
    ctx.screenUV        = screen_uv;
    GetTangentBasis(dir, ctx.Tangent, ctx.Bitangent);
    ctx.lightSizeWorld = lighting_data.light_size_world;
    ctx.shadowMapSize  = lighting_data.shadow_csm_sm_size;
    ctx.clipW          = 1.0;
    ctx.scaleData.x    = near_plane;
    ctx.scaleData.y    = far_plane;
    ctx.normal         = normal;
    ctx.lightDir       = dir;


    if (lighting_data.pcss_enabled == 1) {
        return calculate_pcss(ctx);
    } else {
        if (occluder_depth > fragment_depth + SHADOW_BIAS) {
            return 0.0;
        } else {
            return 1.0;
        }
    }
}

float calculate_csm_shadow(
    Moer::LightingData lighting_data,
    float3             world_pos,
    float2             screen_uv,
    float3             normal
) {
    int cascade_index = get_cascade_index(lighting_data, world_pos);
    if (cascade_index == -1)
        return 1.0;
    float3 main_light_dir = lighting_data.main_light_direction;

    if (lighting_data.is_csm_blend_enabled == 1) {
        float shadow_current =
            get_single_shadow(lighting_data, world_pos, cascade_index, screen_uv, normal, main_light_dir);
        float shadow_next =
            (cascade_index + 1 < lighting_data.shadow_csm_num_of_cascades) ?
                get_single_shadow(
                    lighting_data, world_pos, cascade_index + 1, screen_uv, normal, main_light_dir
                ) :
                1.0;

        float cascade_blend_ratio = get_cascade_blend_ratio(lighting_data, world_pos, cascade_index);
        return lerp(shadow_current, shadow_next, cascade_blend_ratio);
    } else {
        return get_single_shadow(lighting_data, world_pos, cascade_index, screen_uv, normal, main_light_dir);
    }
}

float calculate_shadow(Moer::LightingData lighting_data, float3 world_pos, float2 screen_uv, float3 normal) {
    if (lighting_data.shadow_map_mode == 1) {
        return calculate_point_shadow(lighting_data, world_pos, screen_uv, normal);
    } else {
        return calculate_csm_shadow(lighting_data, world_pos, screen_uv, normal);
    }
}

#endif