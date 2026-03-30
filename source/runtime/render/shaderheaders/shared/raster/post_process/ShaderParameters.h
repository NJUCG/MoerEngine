/**
 * 请统一Include如下文件，不要Include当前文件
 * CPP:
 *     #include "shaderheaders/shared/raster/ShaderParameters.h"
 * HLSL:
 *     #include "shared/raster/ShaderParameters.h"
 */
#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"
namespace Moer::Render {
#else
#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct CameraMotionVectorData {
    float4x4 world2clip;
    float4x4 world2clip_prev;
};

struct AoPipelineBindlessParam {
    float4x4 clip2world;

    float2 inv_resolution;
    float  ssao_intensity;
    float  ssao_max_distance;

    uint ssao_sample_count;
    uint ssao_radius;
    uint ao_mode;
    uint normal_tex;

    uint depth_tex;
    uint noise_tex;             // linear & repeat sampler
    uint camera_mv_data_handle; // for camera motion vector
};

struct SsdoPipelineBindlessParam {
    float4x4 clip2world;
    float4x4 world2clip;

    float3 camera_position;
    float  ssdo_depth_bias;

    float2 inv_resolution;    // 1.0 / (屏幕宽度，高度)
    uint   ssdo_sample_count; // 采样次数 (e.g. 16,32,…)
    float  ssdo_radius;       // 半径（世界空间单位）

    float ssdo_max_distance;       // 最大距离（世界空间单位）
    float ssdo_intensity;          // 强度调节参数
    float ssdo_indirect_intensity; // 间接光强度调节参数
    uint  normal_tex;

    uint depth_tex;
    uint noise_tex;
    uint ao_mode;
    uint input_image;

    uint camera_mv_data_handle;
};

struct RtaoPipelineBindlessParam {
    float4x4 clip2world;
    uint     frame_idx;
    uint     normal_tex;
    uint     depth_tex;
    uint     spp;

    float2 resolution;
    float2 inv_resolution;

    float ray_trace_distance;
    float intensity;
    uint  camera_mv_data_handle;
    uint  noise_tex;

    float2 depth_tex_resolution;
};

struct RtaoDenoiserPassBindlessParam {
    uint history_ao_tex;
    uint curr_ao_tex;
    uint motion_vector_tex;
    uint is_reprojection_enable;

    uint  depth_tex;
    uint  normal_tex;
    uint  is_validation_enable;
    float history_ratio;

    float valid_depth_threshold;
    float valid_normal_threshold;
};

struct AoCompositeParam {
    uint   ao_tex;
    uint   color_tex;
    uint   ao_mode;
    uint   padding0;
    float2 full_resolution;
    float2 inv_full_resolution;
};

struct BilateralFilterDenoiserPipelineBindlessParam {
    float2 inv_resolution;
    float  spatial_sigma_square;
    float  range_sigma_square;

    uint kernel_radius;
    uint input_image;
    uint padding0;
    uint padding1;
};

struct SsrPipelineBindlessParam {
    float4x4 clip2world;
    float4x4 world2clip;

    float3 camera_position;
    float  near_clip;

    float2 resolution;
    float  far_clip;
    float  ssr_roughness_threshold;

    float ssr_metallic_threshold;
    float ssr_step_base;
    uint  ssr_sample_count;
    uint  ssr_is_enable_jitter;
    uint  ssr_is_force_ground_enable_ssr;
    uint  color_tex;
    uint  normal_tex;
    uint  depth_tex;
    uint  vbuffer;
    uint  gbuffer_uv;
    uint  material_buf_hdl;
};

struct SmaaSharedPipelineBindlessParam {
    float4x4 clip2world;
    float4x4 clip2prev_clip; // = previous_view_projection * current_inverse_view_projection
    float4   rt_metrics;     // float4(inv_resolution.xy, resolution.xy)
    uint     aa_mode;
    uint     color_tex; // initial input image
    uint     depth_tex; // depth gbuffer
    uint     search_tex;
    uint     area_tex;
    uint     edges_tex;
    uint     blend_tex;
    uint     current_color_tex;  // current output image (without temporal AA)
    uint     previous_color_tex; // previous output image (without temporal AA)
    uint     frame_index;
    uint     point_sampler;
    uint     linear_sampler;
    uint     padding[3];
};

struct FxaaPrecomputePipelineBindlessParam {
    uint input_image;
};

struct FxaaPipelineBindlessParam {
    uint   input_image;
    uint   fxaa_mode;
    float2 resolution;
    float2 inv_resolution;
};

struct UpsamplePipelineBindlessParam {
    uint input_image;
    uint outSize;
    uint inSize;
    uint upsample_mode;
};

struct AutoExposureParam {
    // *记得对齐*

    // 用于将亮度从[min, max]映射到[0, 1]
    float log2lum_to01_scale;
    float log2lum_to01_bias;
    // 用于将亮度从[0, 1]映射回[min, max]
    float log2lum_to01_scale_inv;
    float log2lum_to01_bias_inv;

    // 只取histogram中[low_percentile, high_percentile]范围内的亮度用于计算平均亮度
    float histogram_low_percentile;  // e.g. 10%
    float histogram_high_percentile; // e.g. 90%
    // 最终曝光值min/max限制
    float min_adapted_luminance;
    float max_adapted_luminance;

    // 眼动适应速度
    float eye_adaptation_speed_up;
    float eye_adaptation_speed_down;
    // 当前帧时间（秒）
    float frame_time;
    // 小于这个值，则直接使用目标曝光值，避免过度缓慢变化
    float diff_log2_threshold;

    // Enabled
    uint enabled;
    uint debug_visualize;
    uint aces_tonemapping_enabled;
    uint padding;
};

struct TonemappingPipelineBindlessParam {
    AutoExposureParam ae;

    uint2  resolution;
    float2 resolution_inv;

    // tonemapping 参数
    float exposure_ev;
    uint  reinhard_enabled;

    float debug_param;
};

static CONST uint TONEMAPPING_HISTOGRAM_GROUP_X = 16;
static CONST uint TONEMAPPING_HISTOGRAM_GROUP_Y = 16;
static CONST uint TONEMAPPING_HISTOGRAM_BIN_COUNT =
    TONEMAPPING_HISTOGRAM_GROUP_X * TONEMAPPING_HISTOGRAM_GROUP_Y;
static CONST uint TONEMAPPING_HISTOGRAM_POINT_FRAC_BITS       = 6;
static CONST uint TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER = 1UL << TONEMAPPING_HISTOGRAM_POINT_FRAC_BITS;

//MARK:Bloom

struct BloomPrefilterParam {
    float threshold;
    float knee;
};

struct BloomDownsampleParam {
    float2 inv_size;
};

struct BloomUpsampleParam {
    float  filter_radius;
    float2 inv_size;
};

struct BloomApplyParam {
    float bloom_intensity;
};

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
//#undef CONST