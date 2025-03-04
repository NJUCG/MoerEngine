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

        float2 color_lut_size;
        float2 color_lut_size_inv;
    };

#ifdef __cplusplus
}
#else
}
#endif

#endif//MOER_SHARED_POSTPROCESS_SHADER_PARAMETERS_H