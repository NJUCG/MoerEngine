#ifndef MOER_LIGHTING_SHADOWS_ENTRY_HLSLI
#define MOER_LIGHTING_SHADOWS_ENTRY_HLSLI

#include "pipelines/raster/deferred/lighting/shadows/CSM.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/PCSS.hlsli"

// --- 变种控制 ---
// 方式 1: 使用宏 (编译时剔除，性能最好，但需要编译多个 Shader Permutation)
// #define USE_PCSS 1 

// 方式 2: 使用 Uniform 分支 (LightingData 中的数据)
// 现代 GPU 对 Uniform 分支优化很好，如果所有像素都走同一个分支，性能损耗很小。
// 你目前的代码使用的是这种方式 (lighting_data.pcss_enabled)。

float get_single_shadow(Moer::LightingData lighting_data, float3 world_pos, int cascade_index,float2 screen_uv, float3 normal, float3 lightDir) {
    float4 shadow_clip_pos = mul(lighting_data.world_to_shadow_clip[cascade_index], float4(world_pos, 1.0));
    float3 shadow_ndc_pos  = shadow_clip_pos.xyz / shadow_clip_pos.w;
    float2 shadow_uv       = float2(shadow_ndc_pos.x * 0.5 + 0.5, 1.0 - (shadow_ndc_pos.y * 0.5 + 0.5));
    bool in_bounds = shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
                     shadow_ndc_pos.z >= 0.0 && shadow_ndc_pos.z <= 1.0;
    if(!in_bounds) {
        return 1.0;
    }

    ShadowContext ctx;
    ctx.shadowMapHandle = lighting_data.shadow_map[cascade_index];
    ctx.shadowUV        = shadow_uv;                               
    ctx.fragmentDepth   = shadow_ndc_pos.z;                        
    ctx.screenUV        = screen_uv;                              
    ctx.lightSizeWorld  = lighting_data.light_size_world;          
    ctx.shadowMapSize   = lighting_data.shadow_csm_sm_size;       
    ctx.clipW           = shadow_clip_pos.w;    
    ctx.scaleData       = lighting_data.scale_data[cascade_index];
    ctx.normal   = normal;
    ctx.lightDir = lightDir;
    

    float occluder_depth =TextureHandle(ctx.shadowMapHandle).Sample2D<float>(shadow_uv).x;
    float fragment_depth = shadow_ndc_pos.z;

#ifdef USE_PCSS_VARIANT
    // 强制使用 PCSS 变种
    return calculate_pcss(ctx);
#else
    if (lighting_data.pcss_enabled == 1) {
        return calculate_pcss(ctx);
    } else {
        // 简单的 PCF 或者 硬阴影
        occluder_depth = TextureHandle(ctx.shadowMapHandle).Sample2D<float>(ctx.shadowUV).x;
        return (fragment_depth + SHADOW_BIAS < occluder_depth) ? 0.0 : 1.0;
    }
#endif
}

float calculate_shadow(Moer::LightingData lighting_data, float3 world_pos, float2 screen_uv, float3 normal) {
    int cascade_index = get_cascade_index(lighting_data, world_pos);
    if (cascade_index == -1)
        return 1.0;
    float3 main_light_dir = lighting_data.main_light_direction;

    if (lighting_data.is_csm_blend_enabled == 1) {
        float shadow_current = get_single_shadow(lighting_data, world_pos, cascade_index, screen_uv, normal, main_light_dir);
        float shadow_next    = (cascade_index + 1 < lighting_data.shadow_csm_num_of_cascades) ?
                                   get_single_shadow(lighting_data, world_pos, cascade_index + 1, screen_uv, normal, main_light_dir) :
                                   1.0;

        float cascade_blend_ratio = get_cascade_blend_ratio(lighting_data, world_pos, cascade_index);
        return lerp(shadow_current, shadow_next, cascade_blend_ratio);
    } else {
        return get_single_shadow(lighting_data, world_pos, cascade_index,screen_uv, normal, main_light_dir);
    }
}

#endif