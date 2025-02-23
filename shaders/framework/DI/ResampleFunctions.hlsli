#ifndef MOER_DI_RESAMPLE_FUNCTIONS_HLSLI
#define MOER_DI_RESAMPLE_FUNCTIONS_HLSLI
#include <framework/DI/Reservoirs.hlsli>
#define NAIVE_SAMPLING_M_THRESHOLD 2
namespace Moer {
namespace DI {
float TargetPdf(const Reservoir _res, const Surface _surface) {
  LightSample l_sample = _surface.SamplePolymorphicLight(
      LoadLightInfo(_res.GetLightIndex()), _res.GetUV());

  return _surface.GetLightSampleTargetPdf(l_sample);
}

struct TemporalResampleParams {
  float3 screen_motion;
  uint src_buffer_idx;
  uint max_history_length;
  uint bias_correction_mode;
  float depth_threshold;
  float normal_threshold;
  bool enable_prior_visibility;
  bool enable_permutation_sampling;
  uint random_seed;
};

Reservoir TemporalResampling(uint2 _pixel_pos, Surface _surface,
                             Reservoir _cur_res, inout RandomState _rng,
                             DI::CommonParams _params,
                             ReservoirBufferParams _reservoir_buffer_params,
                             TemporalResampleParams _t_params,
                             out int2 _temporal_sample_pos,
                             inout LightSample _out_sample) {

  [branch]
  if (_t_params.bias_correction_mode == s_di_bias_correction_pair_wise) {
    _t_params.bias_correction_mode = s_di_bias_correction_basic;
  }

  uint history_limit = min(s_packed_di_reservoir_max_M,
                           uint(_t_params.max_history_length * _cur_res.M));
  int prev_light_id = -1;

  if (_cur_res.IsValid()) {
    prev_light_id = GetMappedLightIndex(_cur_res.GetLightIndex());
  }

  _temporal_sample_pos = int2(-1, -1);
  Reservoir res = Reservoir::EmptyReservoir();
  res.Combine(_cur_res, 0.5f, _cur_res.target_pdf);
  bool b_valid_cur_res = res.IsValid();
  float3 motion = _t_params.screen_motion;

  // permutation sample
  [branch]
  if (!_t_params.enable_permutation_sampling) {
    motion.xy += float2(_rng.GetFloat(), _rng.GetFloat()) - 0.5f;
  }

  float2 reproj_sample_pos = float2(_pixel_pos) + motion.xy;
  int2 prev_pos = int2(round(reproj_sample_pos));
  float expected_depth_linear = _surface.GetLinearDepth() + motion.z;

  Surface temporal_surface = Surface::EmptySurface();
  bool found_neighbor = false;
  const float radius = 8.f;
  int2 spatial_offset = int2(0, 0);

  // search around previous screen position to find a valid neighbor
  [unroll] for (int i = 0; i < 9; i++) {
    int2 offset = 0;
    if (i > 0) {
      offset.x = int((_rng.GetFloat() - 0.5f) * radius);
      offset.y = int((_rng.GetFloat() - 0.5f) * radius);
    }

    int2 idx = prev_pos + offset;
    if (_t_params.enable_permutation_sampling && i == 0) {
      PermutationSampling(idx, _t_params.random_seed);
    }

    temporal_surface = GetGBufferSurface(idx, true);
    if (!temporal_surface.IsValid()) {
      continue;
    }

    // check normal and depth difference
    if (!Math::IsValidNeighbor(
            _surface.GetNormal(), temporal_surface.GetNormal(),
            expected_depth_linear, temporal_surface.GetLinearDepth(),
            _t_params.normal_threshold, _t_params.depth_threshold)) {

      continue;
    }

    spatial_offset = idx - prev_pos;
    prev_pos = idx;
    found_neighbor = true;
    break;
  }

  bool selected_prev_sample = false;
  float prev_M = 0.f;

  if (found_neighbor) {
    uint2 prev_reservoir_pos = prev_pos;
    Reservoir prev_res = LoadReservoir(
        _reservoir_buffer_params, prev_reservoir_pos, _t_params.src_buffer_idx);
    prev_res.M = min(prev_res.M, history_limit);
    prev_res.spatial_dist += spatial_offset;
    prev_res.age += 1;

    uint original_light_idx = prev_res.GetLightIndex();
    if (prev_res.IsValid()) {
      if (prev_res.age <= 1) {
        _temporal_sample_pos = prev_pos;
      }

      int mapped_light_idx = GetMappedLightIndex(original_light_idx);
      if (mapped_light_idx < 0) {
        prev_res.weight_sum = 0.f;
        prev_res.light_data = 0;
      } else {
        prev_res.light_data = mapped_light_idx | s_di_reservoir_light_valid_bit;
      }
    }
    prev_M = prev_res.M;
    float current_weight = 0.f;
    LightSample candidate = LightSample::EmptyLightSample();
    if (prev_res.IsValid()) {
      PolymorphicLightInfo light_info = LoadLightInfo(prev_res.GetLightIndex());
      candidate = _surface.SamplePolymorphicLight(light_info, prev_res.GetUV());
      current_weight = _surface.GetLightSampleTargetPdf(candidate);
    }

    bool selected = res.Combine(prev_res, _rng.GetFloat(), current_weight);
    if (selected) {
      selected_prev_sample = true;
      prev_light_id = int(original_light_idx);
      _out_sample = candidate;
    }
  }

  [branch]
  if (_t_params.bias_correction_mode >= s_di_bias_correction_basic) {

    float pi = res.target_pdf;
    float pi_sum = res.target_pdf * _cur_res.M;

    if (res.IsValid() && prev_light_id >= 0 && prev_M > 0) {
      float temporal_p = 0.f;
      PolymorphicLightInfo selected_prev = LoadLightInfo(prev_light_id);
      LightSample selected_temporal =
          temporal_surface.SamplePolymorphicLight(selected_prev, res.GetUV());
      temporal_p = temporal_surface.GetLightSampleTargetPdf(selected_temporal);

      if (_t_params.bias_correction_mode == s_di_bias_correction_traced &&
          (!selected_prev_sample || !_t_params.enable_prior_visibility)) {
        if (!GetPreviousConservativeVisibility(temporal_surface,
                                               selected_temporal.x)) {
          temporal_p = 0.f;
        }
      }

      pi = selected_prev_sample ? temporal_p : pi;
      pi_sum += temporal_p * prev_M;
    }

    res.FinalizeRIS(pi, pi_sum);
  } else {
    res.FinalizeRIS(1.f, res.M);
  }

  return res;
}
} // namespace DI
} // namespace Moer
#endif // MOER_DI_RESAMPLE_FUNCTIONS_HLSLI