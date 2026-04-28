#ifndef MOER_LIGHTING_SHADOWS_CSM_HLSLI
#define MOER_LIGHTING_SHADOWS_CSM_HLSLI

#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

// 获取Cascade Index（同时输出 view-space Z，避免重复矩阵乘法）
int get_cascade_index(Moer::LightingData lighting_data, float3 world_pos, out float out_view_z) {
    out_view_z = abs(mul(lighting_data.world2view, float4(world_pos, 1.0)).z);
    float pixel_depth_ratio =
        (out_view_z - lighting_data.near_clip) / (lighting_data.far_clip - lighting_data.near_clip);
    for (int i = 0; i < lighting_data.shadow_csm_num_of_cascades; i++) {
        if (pixel_depth_ratio < lighting_data.cascade_split_ratios[i]) {
            return i;
        }
    }
    return -1;
}

// 返回每层 CSM 调试可视化使用的固定颜色。
float3 get_cascade_visualize_base_color(int cascade_index) {
    static const float3 cascade_colors[4] = {
        float3(0.96, 0.24, 0.24),
        float3(0.96, 0.58, 0.18),
        float3(0.25, 0.78, 0.32),
        float3(0.20, 0.45, 0.96)
    };
    return cascade_colors[clamp(cascade_index, 0, 3)];
}

float get_cascade_blend_ratio(Moer::LightingData lighting_data, float pixel_view_pos_z, int cascade_index) {
    float blend_band_start_z =
        lighting_data.near_clip + lighting_data.cascade_blend_start_ratios[cascade_index] *
                                      (lighting_data.far_clip - lighting_data.near_clip);
    float blend_band_end_z = lighting_data.near_clip + lighting_data.cascade_split_ratios[cascade_index] *
                                                           (lighting_data.far_clip - lighting_data.near_clip);
    return smoothstep(blend_band_start_z, blend_band_end_z, pixel_view_pos_z);
}

// 生成 CSM false-color，可在 blend 区域平滑过渡到下一层颜色。
float3 get_cascade_visualize_color(Moer::LightingData lighting_data, float3 world_pos) {
    float pixel_view_z;
    int cascade_index = get_cascade_index(lighting_data, world_pos, pixel_view_z);
    if (cascade_index < 0) {
        return float3(0.05, 0.05, 0.05);
    }

    float3 cascade_color = get_cascade_visualize_base_color(cascade_index);
    if (lighting_data.is_csm_blend_enabled == 1 && cascade_index + 1 < lighting_data.shadow_csm_num_of_cascades) {
        float blend_ratio = get_cascade_blend_ratio(lighting_data, pixel_view_z, cascade_index);
        if (blend_ratio > 0.0) {
            float3 next_cascade_color = get_cascade_visualize_base_color(cascade_index + 1);
            return lerp(cascade_color, next_cascade_color, blend_ratio);
        }
    }

    return cascade_color;
}

#endif