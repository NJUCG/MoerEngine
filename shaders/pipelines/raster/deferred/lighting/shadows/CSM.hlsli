#ifndef MOER_LIGHTING_SHADOWS_CSM_HLSLI
#define MOER_LIGHTING_SHADOWS_CSM_HLSLI

#include "core/common/Common.hlsl"
#include "shared/raster/ShaderParameters.h"

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
        abs(mul(lighting_data.view_matrix, float4(world_pos, 1.0)).z); //FIXME:需要取负吗？
    float blend_band_start_z =
        lighting_data.near_clip + lighting_data.cascade_blend_start_ratios[cascade_index] *
                                      (lighting_data.far_clip - lighting_data.near_clip);
    float blend_band_end_z = lighting_data.near_clip + lighting_data.cascade_split_ratios[cascade_index] *
                                                           (lighting_data.far_clip - lighting_data.near_clip);
    return smoothstep(blend_band_start_z, blend_band_end_z, pixel_view_pos_z);
}

#endif