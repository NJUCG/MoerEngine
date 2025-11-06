#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
//#define CONST constexpr
#include "misc/Traits.h"
#include "shaderheaders/shared/raster/ShaderParametersUtils.h"
namespace Moer::Render {
#else
#include "shared/raster/ShaderParametersUtils.h"
//#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct CameraMotionVectorData {
    float4x4 world2clip;
    float4x4 world2clip_prev;
};

struct AoPipelineBindlessParam {
    float2 inv_resolution;
    float  ssao_intensity;
    float  ssao_max_distance;

    uint ssao_sample_count;
    uint ssao_radius;
    uint ao_mode;
    uint input_image;

    uint normal_tex;
    uint position_tex;
    uint depth_tex;
    uint noise_tex; // linear & repeat sampler

    uint camera_mv_data_handle; // for camera motion vector
};

struct SsdoPipelineBindlessParam {
    float2 inv_resolution;          // 1.0 / (屏幕宽度，高度)
    uint   ssdo_sample_count;       // 采样次数 (e.g. 16,32,…)
    float  ssdo_radius;             // 半径（世界空间单位）
    float  ssdo_max_distance;       // 最大距离（世界空间单位）
    float  ssdo_intensity;          // 强度调节参数
    float  ssdo_indirect_intensity; // 间接光强度调节参数

    uint normal_tex;
    uint depth_tex;
    uint position_tex;
    uint noise_tex;

    uint ao_mode;

    uint     input_image;
    float4x4 view_projection_matrix;
    float4x4 view_matrix;
    float3   camera_position;
    float    ssdo_depth_bias;

    uint camera_mv_data_handle;
};

struct RtaoPipelineBindlessParam {
    float4x4 clip2world;
    float3   camera_pos;
    uint     frame_idx;

    float2 resolution;
    float2 inv_resolution;

    uint input_image;
    uint normal_tex;
    uint position_tex;
    uint depth_tex;

    uint  ao_mode;
    uint  sample_mode;
    uint  spp;
    float ray_trace_distance;
    float intensity;

    uint camera_mv_data_handle; // for camera motion vector
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
    float4x4 view_projection_matrix;
    float3   camera_position;
    float    near_clip;
    float2   resolution;
    float    far_clip;
    float    ssr_roughness_threshold;
    float    ssr_metallic_threshold;
    float    ssr_step_base;
    uint     ssr_sample_count;
    uint     ssr_is_enable_jitter;
    uint     ssr_is_force_ground_enable_ssr;
    uint     color_tex;
    uint     position_tex;
    uint     normal_tex;
    uint     depth_tex;
    uint     vbuffer;
    uint     gbuffer_uv;
    uint     material_buffer;
};

struct SmaaSharedPipelineBindlessParam {
    float4x4 curr_inv_vp_and_prev_vp; // = previous_view_projection * current_inverse_view_projection
    float4   rt_metrics;              // float4(inv_resolution.xy, resolution.xy)
    uint     aa_mode;
    uint     color_tex;    // initial input image
    uint     position_tex; // position gbuffer
    uint     depth_tex;    // depth gbuffer
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

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
//#undef CONST

//Enum Definitions Begin
namespace Moer {
EnumParam(EAaMode, NONE, FXAA_SIMPLIFIED, FXAA_QUALITY, SMAA_1X, SMAA_T2X);
EnumParam(EAoMode, NONE, SSAO, SSAO_AO_ONLY, RTAO, RTAO_AO_ONLY, SSDO, SSDO_AO_ONLY, LINEARIZED_DEPTH_DIV_10);
EnumParam(EDenoiserMode, NONE, BILATERAL_FILTER);
EnumParam(ERtaoSampleMode, UNIFORM, COSINE_WEIGHTED);
} // namespace Moer