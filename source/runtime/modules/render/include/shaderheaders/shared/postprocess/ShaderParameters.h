#ifndef MOER_SHARED_POSTPROCESS_SHADER_PARAMETERS_H
#define MOER_SHARED_POSTPROCESS_SHADER_PARAMETERS_H

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif
#define HISTOGRAM_BINS 256

    struct ToneMappingParams {
        uint2 view_origin;
        uint2 view_size;

        float2 color_lut_size;
        float2 color_lut_size_inv;

        float log_luminance_scale;
        float log_luminance_bias;
        float histogram_low_percentile;
        float histogram_high_percentile;

        float eye_adaptation_speed_up;
        float eye_adaptation_speed_down;
        float min_adapted_luminance;
        float max_adapted_luminance;

        float frame_time;
        float exposure_scale;
        float white_point_inv_squared;
        uint  source_slice;

        float log_luminance_scale_exposure;
        float log_luminance_bias_exposure;
        float padding[2];
    };

    struct TAAParams {
        float4x4 reprojection_matrix;

        float2 in_view_origin;
        float2 in_view_size;
        float2 out_view_origin;
        float2 out_view_size;

        float2 in_pixel_offset;
        float2 out_texture_size_inv;

        float2 input_over_output_size;
        float2 output_over_input_size;

        float clamping_factor;
        float new_frame_weight;
        float pqc;
        float inv_pqc;
    };

#ifdef __cplusplus
}
#else
}
#endif

#endif//MOER_SHARED_POSTPROCESS_SHADER_PARAMETERS_H