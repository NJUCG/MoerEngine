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

//////////////////////////////////////////////////////////////////////////
// Temporal Resampling
//////////////////////////////////////////////////////////////////////////
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

  [branch] if (_t_params.bias_correction_mode ==
               s_di_bias_correction_pair_wise) {
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

  //DEBUG
  // res.FinalizeRIS(1.f, _cur_res.M);
  // return res;

  bool b_valid_cur_res = res.IsValid();
  float3 motion = _t_params.screen_motion;

  // permutation sample
  [branch] if (!_t_params.enable_permutation_sampling) {
    motion.xy += float2(_rng.GetFloat(), _rng.GetFloat()) - 0.5f;
  }

  float2 reproj_sample_pos = float2(_pixel_pos) + motion.xy;
  int2 prev_pos = int2(round(reproj_sample_pos));
  float expected_depth_linear = _surface.GetLinearDepth() + motion.z;

  Surface temporal_surface = Surface::EmptySurface();
  bool found_neighbor = false;
  const float radius = 4.f;
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

  [branch] if (_t_params.bias_correction_mode >= s_di_bias_correction_basic) {

    float pi = res.target_pdf;
    float pi_sum = res.target_pdf * _cur_res.M;

    if (res.IsValid() && prev_light_id >= 0 && prev_M > 0) {
      float temporal_p = 0.f;
      PolymorphicLightInfo selected_prev = LoadLightInfo(prev_light_id);
      LightSample selected_temporal =
          temporal_surface.SamplePolymorphicLight(selected_prev, res.GetUV());
      temporal_p = temporal_surface.GetLightSampleTargetPdf(selected_temporal);

      if (_t_params.bias_correction_mode >= s_di_bias_correction_traced &&
          temporal_p > 0.f &&
          (!selected_prev_sample || !_t_params.enable_prior_visibility)) {
        if (!GetPreviousConservativeVisibility(_surface,
                                               selected_temporal.x)) {
          temporal_p = 0.f;
        }
      }

      pi = selected_prev_sample ? temporal_p : pi;
      pi_sum += temporal_p * prev_M;
    }

    res.FinalizeRIS(pi, pi_sum);
  }
  else {
    res.FinalizeRIS(1.f, res.M);
  }

  return res;
}

//////////////////////////////////////////////////////////////////////////
// Spatial Resampling
//////////////////////////////////////////////////////////////////////////
float PairWiseMisWeight(float _w0, float _w1, float _M0, float _M1) {
  // balanced heuristic
  float denom = _w0 * _M0 + _w1 * _M1;
  return denom > 0.f ? max((_w0 * _M0), 0.f) / denom : 0.f;
}

float MFactor(float _q0, float _q1) {
  return (_q0 <= 0.0f) ? 1.0f
                       : clamp(pow(min(_q1 / _q0, 1.0f), 8.0f), 0.0f, 1.0f);
}

bool StreamNeighborWithPairwiseMIS(inout Reservoir _res, float _rnd,
                                   const Reservoir _neighbor_res,
                                   const Surface _neigbor_surface,
                                   const Reservoir _canonical_res,
                                   const Surface _canonical_surface,
                                   const uint _num_neighbors) {

  float neighbor_weight_at_canonical =
      max(0.f, TargetPdf(_neighbor_res, _canonical_surface));
  float canonical_weight_at_neighbor =
      max(0.f, TargetPdf(_canonical_res, _neigbor_surface));
  float neighbor_weight_at_neighbor =
      max(0.f, TargetPdf(_neighbor_res, _neigbor_surface));
  float canonical_weight_at_canonical =
      max(0.f, TargetPdf(_canonical_res, _canonical_surface));

  // neighbor weight sum factor
  float w0 = PairWiseMisWeight(
      neighbor_weight_at_neighbor, neighbor_weight_at_canonical,
      _neighbor_res.M * _num_neighbors, _canonical_res.M);

  // canonical weight sum factor
  float w1 = PairWiseMisWeight(
      canonical_weight_at_neighbor, canonical_weight_at_canonical,
      _neighbor_res.M * _num_neighbors, _canonical_res.M);

  // Symmetric Weight
  float M =
      _neighbor_res.M *
      min(MFactor(neighbor_weight_at_neighbor, neighbor_weight_at_canonical),
          MFactor(canonical_weight_at_neighbor, canonical_weight_at_canonical));

  // overweight canonical sample _num_neighbors times
  _res.canonical_weight += (1.f - w1);
  return _res.Resample(_neighbor_res, _rnd, neighbor_weight_at_canonical,
                       w0 * _neighbor_res.weight_sum, M);
}

bool StreamCanonicalWithPairwiseMIS(inout Reservoir _res, float _rnd,
                                    const Reservoir _canonical_res,
                                    const Surface _canonical_surface) {

  return _res.Resample(_canonical_res, _rnd, _canonical_res.target_pdf,
                       _canonical_res.weight_sum * _res.canonical_weight,
                       _canonical_res.M);
}

struct SpatialResampleParams {
  uint src_buffer_idx;
  uint num_samples;
  uint num_disocclusion_samples;
  uint max_history_length;
  uint bias_correction_mode;
  float sampling_radius;
  float depth_threshold;
  float normal_threshold;
  bool b_test_material_similarity;
  bool discount_native_samples;
};

Reservoir SpatialResampleWithPairwiseMIS(
    uint2 _pixel_pos, Surface _surface, Reservoir _cur_res,
    inout RandomState _rng, DI::CommonParams _params,
    ReservoirBufferParams _reservoir_buffer_params,
    SpatialResampleParams _s_params, inout LightSample _out_sample) {

  Reservoir res = Reservoir::EmptyReservoir();
  res.canonical_weight = 0.f;

  uint num_samples =
      (_cur_res.M < _s_params.max_history_length)
          ? max(_s_params.num_disocclusion_samples, _s_params.num_samples)
          : _s_params.num_samples;

  uint start_idx = uint(_rng.GetFloat() * _params.neighbor_offset_mask);
  uint num_valid_samples = 0;
  uint i;

  ArrayBuffer neighbor_offset_buf =
      (ArrayBuffer)resample_params.bindless_handles.neighbor_offset;

  for (i = 0; i < num_samples; i++) {
    uint sample_idx = (start_idx + i) & _params.neighbor_offset_mask;
    int2 spatial_offset = int2(neighbor_offset_buf.Load<float2>(sample_idx) *
                               _s_params.sampling_radius);
    int2 idx = int2(_pixel_pos) + spatial_offset;
    idx = ClampScreenPosition(idx);

    Surface neighbor_surface = GetGBufferSurface(idx);

    if (!neighbor_surface.IsValid()) {
      continue;
    }

    if (!Math::IsValidNeighbor(
            _surface.GetNormal(), neighbor_surface.GetNormal(),
            _surface.GetLinearDepth(), neighbor_surface.GetLinearDepth(),
            _s_params.normal_threshold, _s_params.depth_threshold)) {
      continue;
    }

    if (_s_params.b_test_material_similarity &&
        !_surface.HasSimilarMaterial(neighbor_surface)) {
      continue;
    }

    Reservoir neighbor_res =
        LoadReservoir(_reservoir_buffer_params, idx, _s_params.src_buffer_idx);

    if (neighbor_res.IsValid()) {
      if (_s_params.discount_native_samples &&
          neighbor_res.M < NAIVE_SAMPLING_M_THRESHOLD)
        continue;
    }
    num_valid_samples++;

    if (neighbor_res.M <= 0) {
      continue;
    }

    StreamNeighborWithPairwiseMIS(res, _rng.GetFloat(), neighbor_res,
                                  neighbor_surface, _cur_res, _surface,
                                  num_samples);
  }

  res.canonical_weight = (num_valid_samples > 0) ? res.canonical_weight : 1.f;

  StreamCanonicalWithPairwiseMIS(res, _rng.GetFloat(), _cur_res, _surface);
  res.FinalizeRIS(1.f, float(max(1, num_valid_samples)));

  _out_sample = _surface.SamplePolymorphicLight(
      LoadLightInfo(res.GetLightIndex()), res.GetUV());
  return res;
}

Reservoir SpatialResampling(uint2 _pixel_pos, Surface _surface,
                            Reservoir _cur_res, inout RandomState _rng,
                            DI::CommonParams _params,
                            ReservoirBufferParams _reservoir_buffer_params,
                            SpatialResampleParams _s_params,
                            inout LightSample _out_sample) {

  if (_s_params.bias_correction_mode == s_di_bias_correction_pair_wise) {
    return SpatialResampleWithPairwiseMIS(_pixel_pos, _surface, _cur_res, _rng,
                                          _params, _reservoir_buffer_params,
                                          _s_params, _out_sample);
  }

  Reservoir res = Reservoir::EmptyReservoir();

  int selected = -1;
  PolymorphicLightInfo selected_light_info = EmptyLightInfo();

  if (_cur_res.IsValid()) {
    selected_light_info = LoadLightInfo(_cur_res.GetLightIndex());
  }

  res.Combine(_cur_res, 0.5f, _cur_res.target_pdf);
  // res.FinalizeRIS(1.f, _cur_res.M);
  // return res;
  uint start_idx = uint(_rng.GetFloat() * _params.neighbor_offset_mask);

  uint num_samples =
      _cur_res.M < _s_params.max_history_length
          ? max(_s_params.num_disocclusion_samples, _s_params.num_samples)
          : _s_params.num_samples;

  num_samples = min(num_samples, 32);
  uint result_mask = 0;
  uint i;
  ArrayBuffer neighbor_offset_buf =
      (ArrayBuffer)resample_params.bindless_handles.neighbor_offset;
  // two iteration over the samples
  for (i = 0; i < num_samples; i++) {
    uint sample_idx = (start_idx + i) & _params.neighbor_offset_mask;
    int2 spatial_offset = int2(neighbor_offset_buf.Load<float2>(sample_idx) *
                               _s_params.sampling_radius);
    int2 idx = int2(_pixel_pos) + spatial_offset;
    int2 test_idx = idx;
    idx = ClampScreenPosition(idx);

    Surface neighbor_surface = GetGBufferSurface(idx);

    if (!neighbor_surface.IsValid()) {
      continue;
    }

    if (!Math::IsValidNeighbor(
            _surface.GetNormal(), neighbor_surface.GetNormal(),
            _surface.GetLinearDepth(), neighbor_surface.GetLinearDepth(),
            _s_params.normal_threshold, _s_params.depth_threshold)) {
      continue;
    }

    if (_s_params.b_test_material_similarity &&
        !_surface.HasSimilarMaterial(neighbor_surface)) {
      continue;
    }

    Reservoir neighbor_res =
        LoadReservoir(_reservoir_buffer_params, idx, _s_params.src_buffer_idx);
    result_mask |= 1u << i;

    neighbor_res.spatial_dist += spatial_offset;

    PolymorphicLightInfo light_info = EmptyLightInfo();
    float neighbor_weight = 0.f;
    LightSample l_sample = LightSample::EmptyLightSample();

    if (neighbor_res.IsValid()) {
      if (_s_params.discount_native_samples &&
          neighbor_res.M < NAIVE_SAMPLING_M_THRESHOLD) {
        continue;
      }

      light_info = LoadLightInfo(neighbor_res.GetLightIndex());
      l_sample =
          _surface.SamplePolymorphicLight(light_info, neighbor_res.GetUV());
      neighbor_weight = _surface.GetLightSampleTargetPdf(l_sample);


    }

    if (res.Combine(neighbor_res, _rng.GetFloat(), neighbor_weight)) {
      selected = int(i);
      selected_light_info = light_info;
      _out_sample = l_sample;
    }

  }

  if (res.IsValid()) {
    if (_s_params.bias_correction_mode >= s_di_bias_correction_basic) {
      float pi = res.target_pdf;
      float pi_sum = res.target_pdf * _cur_res.M;

      // second iteration to update visibility
      for (i = 0; i < num_samples; i++) {
        if ((result_mask & (1u << uint(i))) == 0) {
          continue;
        }
        uint sample_idx = (start_idx + i) & _params.neighbor_offset_mask;
        int2 spatial_offset = int2(neighbor_offset_buf.Load<float2>(sample_idx) *
                                  _s_params.sampling_radius);

        int2 idx = int2(_pixel_pos) + spatial_offset;
        idx = ClampScreenPosition(idx);

        Surface neighbor_surface = GetGBufferSurface(idx);

        LightSample neighbor_sample = _surface.SamplePolymorphicLight(
            selected_light_info, res.GetUV());
        float ps = neighbor_surface.GetLightSampleTargetPdf(neighbor_sample);

        if (_s_params.bias_correction_mode >= s_di_bias_correction_traced &&
            ps > 0.f) {
          if (!GetCurrentConservativeVisibility(_surface, neighbor_sample.x)) {
            ps = 0.f;
          }
        }

        Reservoir neighbor_res = LoadReservoir(_reservoir_buffer_params, idx,
                                               _s_params.src_buffer_idx);

        // if(!neighbor_res.IsValid() && ps > 0.f) {
        //   // printf("neighbor_res is invalid\n");
        //   continue;
        // }
        pi = selected == i ? ps : pi;
        pi_sum += ps * neighbor_res.M;
      }

      res.FinalizeRIS(pi, pi_sum);
    } else {
      res.FinalizeRIS(1.f, res.M);
    }
  }

  return res;
}
} // namespace DI
} // namespace Moer
#endif // MOER_DI_RESAMPLE_FUNCTIONS_HLSLI