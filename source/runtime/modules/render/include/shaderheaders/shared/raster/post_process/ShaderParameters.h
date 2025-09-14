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

    struct AoPipelineBindlessParam {
        float2 inv_resolution;
        float  ssao_intensity;
        float  ssao_max_distance;
        uint   ssao_sample_count;
        uint   ssao_radius;
        uint   ao_mode;
        uint   input_image;
        uint   normal_tex;
        uint   position_tex;
        uint   noise_tex;// linear & repeat sampler
    };

    struct RtaoPipelineBindlessParam {
        float4x4 clip2world;

        float3 camera_pos;
        uint   frame_idx;

        float2 resolution;
        float2 inv_resolution;

        uint input_image;
        uint normal_tex;
        uint position_tex;
        uint ao_mode;

        uint  sample_mode;
        uint  spp;
        float ray_trace_distance;
        float intensity;
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
        float4x4 curr_inv_vp_and_prev_vp;// = previous_view_projection * current_inverse_view_projection
        float4   rt_metrics;             // float4(inv_resolution.xy, resolution.xy)
        uint     aa_mode;
        uint     color_tex;   // initial input image
        uint     position_tex;// position gbuffer
        uint     depth_tex;   // depth gbuffer
        uint     search_tex;
        uint     area_tex;
        uint     edges_tex;
        uint     blend_tex;
        uint     current_color_tex; // current output image (without temporal AA)
        uint     previous_color_tex;// previous output image (without temporal AA)
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

    // MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
