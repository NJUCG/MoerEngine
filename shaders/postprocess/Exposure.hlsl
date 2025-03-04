#include <shared/postprocess/ShaderParameters.h>

[[vk::binding(0, 0)]] ConstantBuffer<Moer::ToneMappingParams> params
    : register(b0);
[[vk::binding(1, 0)]] Buffer<uint> histogram : register(t0);
[[vk::binding(2, 0)]] RWBuffer<uint> exposure : register(u0);
#define POINT_FRAC_BITS 6
#define POINT_FRAC_MULTIPLIER (1 << POINT_FRAC_BITS)

[numthreads(1, 1, 1)]
void main() {
      float cdf = 0.0f;
      uint i;

      [loop] for (i = 0; i < HISTOGRAM_BINS; i++) {
        cdf += float(histogram[i]) / POINT_FRAC_MULTIPLIER;
      }
      float low_cdf = cdf * params.histogram_low_percentile;
      float high_cdf = cdf * params.histogram_high_percentile;

      float weight_sum = 0.0f;
      float bin_sum = 0.0f;
      cdf = 0.0f;

      [loop] for (i = 0; i < HISTOGRAM_BINS; i++) {
        float bin_val = float(histogram[i]) / POINT_FRAC_MULTIPLIER;
        if (low_cdf <= cdf + bin_val && cdf < high_cdf) {
          float histogram_bin_luminance =
              exp2((i / (float)HISTOGRAM_BINS) * params.log_luminance_scale +
                   params.log_luminance_bias);
          weight_sum += histogram_bin_luminance * bin_val;
          bin_sum += bin_val;
        }
        cdf += bin_val;
      }

      float target_exposure = (bin_sum > 0) ? (weight_sum / bin_sum) : 0.0f;

      target_exposure = clamp(target_exposure, params.min_adapted_luminance,
                              params.max_adapted_luminance);

      float old_exposure = asfloat(exposure[0]);
      float diff = old_exposure - target_exposure;

      float adaption_speed = (diff < 0) ? params.eye_adaptation_speed_up
                                        : params.eye_adaptation_speed_down;
      if (adaption_speed > 0.0f) {
        target_exposure += diff * exp2(-params.frame_time * adaption_speed);
      }
      if(target_exposure <= 0.0f) {
        printf("target exposure is %f, old exposure %f frame time %f\n",target_exposure, old_exposure, params.frame_time);
      }

      exposure[0] = asuint(target_exposure);
    }