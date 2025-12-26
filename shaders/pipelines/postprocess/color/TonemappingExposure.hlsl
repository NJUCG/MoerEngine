#include "core/math/STL.hlsli"
#include "core/math/Math.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Buffer<uint>   histogram;
[[vk::binding(1, 0)]] RWBuffer<uint> exposure;

[numthreads(1, 1, 1)] void main() {
    // 1. Calculate CDF

    float cdf = 0.0f;
    uint  i;

    [loop] for (i = 0; i < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT; i++) {
        cdf += float(histogram[i]) / Moer::TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER;
    }
    float low_cdf  = cdf * param.ae.histogram_low_percentile;
    float high_cdf = cdf * param.ae.histogram_high_percentile;

    // 2. Calculate target exposure

    float weight_sum = 0.0f;
    float bin_sum    = 0.0f;
    cdf              = 0.0f;

    [loop] for (i = 0; i < Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT; i++) {
        // bin_val: 像素数量
        float bin_val = float(histogram[i]) / Moer::TONEMAPPING_HISTOGRAM_POINT_FRAC_MULTIPLIER;

        // 下面这个公式，是在判断区间相交。即 [low_cdf, high_cdf] 和 [cdf, cdf + bin_val] 是否相交
        if (low_cdf <= cdf + bin_val && cdf < high_cdf) {

            // x in [0, 1]
            float x = i / (float)Moer::TONEMAPPING_HISTOGRAM_BIN_COUNT;

            // y in [log2lum_min, log2lum_max], e.g. [-10, 16]
            float y = x * param.ae.log2lum_to01_scale_inv + param.ae.log2lum_to01_bias_inv;

            // lum in [2^log2lum_min, 2^log2lum_max], e.g. [0.001, 65536]
            float lum = exp2(y);

            weight_sum += lum * bin_val;
            bin_sum += bin_val;
        }

        cdf += bin_val;
    }

    // 此处
    // cdf = 总像素数
    // bin_sum in [0, cdf]: 有效像素数
    // weight_sum in [0, cdf * 2^log2lum_max]: 有效像素的Luminance加权和

    // target_exposure in [2^log2lum_min, 2^log2lum_max]
    float target_exposure = (bin_sum > 0) ? (weight_sum / bin_sum) : 0.0f;

    // 3. Adapt exposure

    target_exposure = clamp(target_exposure, param.ae.min_adapted_luminance, param.ae.max_adapted_luminance);

    // 需要在log2空间内进行插值，不能在线性空间插值，否则会导致曝光上升速度极快
    // - RT是在线性空间内插值的，效果不够好
    // - Raster改为log2空间内插值，效果更自然
    float last_exposure    = asfloat(exposure[0]);
    float last_exposure_log2 = log2(max(last_exposure, 0.0001f));
    float target_exposure_log2  = log2(max(target_exposure, 0.0001f));
    float diff_log2          = target_exposure_log2 - last_exposure_log2;

    if (abs(diff_log2) < param.ae.diff_log2_threshold) {
        exposure[0] = asuint(target_exposure);
        return;
    }

    float adaption_speed = (diff_log2 > 1) ? param.ae.eye_adaptation_speed_up : param.ae.eye_adaptation_speed_down;
    float delta_log2 = diff_log2 * (param.ae.frame_time * adaption_speed);

    float result_exposure_log2 = last_exposure_log2 + delta_log2;
    float result_exposure = exp2(result_exposure_log2);

    // printf("target exposure is %f, current %f, last %f, diff(log2) %f, delta(log2) %f, frame time %f, adaption speed %f\n",
    //        target_exposure,
    //        result_exposure,
    //        last_exposure,
    //        diff_log2,
    //        delta_log2,
    //        param.ae.frame_time,
    //        adaption_speed);

    exposure[0] = asuint(result_exposure);
}