#ifndef MOER_FRAMEWORK_DI_RESERVOIRS_HLSLI
#define MOER_FRAMEWORK_DI_RESERVOIRS_HLSLI
#include <framework/DI/Utils.hlsli>
#include <shared/lighting/ShaderParameters.h>

namespace Moer {
namespace DI {
// Encoding helper constants for PackedDIReservoir.visibility
static const uint s_packed_di_reservoir_visibility_mask = 0x3ffff;
static const uint s_packed_di_reservoir_visibility_channel_max = 0x3f;
static const float s_packed_di_reservoir_visibility_channel_max_inv =
    1.f / 0x3f;
static const uint s_packed_di_reservoir_visibility_channel_shift = 6;
static const uint s_packed_di_reservoir_M_shift = 18;
static const uint s_packed_di_reservoir_max_M = 0x3fff;

// Encoding helper constants for PackedDIReservoir.distance_age
static const uint s_packed_di_reservoir_distance_channel_bits = 8;
static const uint s_packed_di_reservoir_distance_x_shift = 0;
static const uint s_packed_di_reservoir_distance_y_shift = 8;
static const uint s_packed_di_reservoir_age_shift = 16;
static const uint s_packed_di_reservoir_max_age = 0xff;
static const uint s_packed_di_reservoir_distance_mask =
    (1u << s_packed_di_reservoir_distance_channel_bits) - 1;
static const int s_packed_di_reservoir_max_distance =
    int((1u << (s_packed_di_reservoir_distance_channel_bits - 1)) - 1);

// Light index helpers
static const uint s_di_reservoir_light_valid_bit = 0x80000000;
static const uint s_di_reservoir_light_index_mask = 0x7FFFFFFF;

struct VisibilityResuseParam {
  uint max_age;
  float max_distance;
};
struct Reservoir {
  uint light_data;
  uint uv_data;
  float weight_sum;
  float target_pdf;
  float M;
  uint packed_visibility;
  int2 spatial_dist;
  uint age;               // frame
  float canonical_weight; // Cannonical weight when using pairwise MIS

  static Reservoir EmptyReservoir() {
    Reservoir r;
    r.light_data = 0;
    r.uv_data = 0;
    r.weight_sum = 0;
    r.target_pdf = 0;
    r.M = 0;
    r.packed_visibility = 0;
    r.spatial_dist = int2(0, 0);
    r.age = 0;
    r.canonical_weight = 0;
    return r;
  }
  bool GetVisibility(const VisibilityResuseParam _param,
                     out float3 _visibility) {
    if (age > 0 && age < _param.max_age &&
        length(float2(spatial_dist)) < _param.max_distance) {
      _visibility =
          float3(float(packed_visibility &
                       s_packed_di_reservoir_visibility_channel_max) *
                     s_packed_di_reservoir_visibility_channel_max_inv,
                 float((packed_visibility >>
                        s_packed_di_reservoir_visibility_channel_shift) &
                       s_packed_di_reservoir_visibility_channel_max) *
                     s_packed_di_reservoir_visibility_channel_max_inv,
                 float((packed_visibility >>
                        (s_packed_di_reservoir_visibility_channel_shift * 2)) &
                       s_packed_di_reservoir_visibility_channel_max) *
                     s_packed_di_reservoir_visibility_channel_max_inv);
      return true;
    }

    _visibility = float3(0, 0, 0);
    return false;
  }

  uint GetLightIndex() { return light_data & s_di_reservoir_light_index_mask; }

  bool IsValid() { return light_data != 0; }

  float2 GetUV() {
    return float2((uv_data & 0xffff) / float(0xffff), (uv_data >> 16) / 0xffff);
  }
  float GetInvPdf() { return weight_sum; }

  bool StreamSample(uint _light_idx, float2 _uv, float _random,
                    float _target_pdf, float _inv_src_pdf) {
    float ris_weight = _target_pdf * _inv_src_pdf;
    M += 1;
    weight_sum += ris_weight;
    bool select = (_random * weight_sum) < ris_weight;

    if (select) {
      light_data = _light_idx | s_di_reservoir_light_valid_bit;
      uv_data = uint(saturate(_uv.x) * 0xffff) |
                (uint(saturate(_uv.y) * 0xffff) << 16);
      target_pdf = _target_pdf;
    }

    return select;
  }

  bool Resample(const Reservoir _other, float _random, float _target_pdf = 1.f,
                float _normalization = 1.f, float _M = 1.f) {
    float ris_weight = _target_pdf * _normalization;
    M += _M;
    weight_sum += ris_weight;

    bool select = (_random * weight_sum) < ris_weight;
    if (select) {
      light_data = _other.light_data;
      uv_data = _other.uv_data;
      target_pdf = _target_pdf;
      packed_visibility = _other.packed_visibility;
      spatial_dist = _other.spatial_dist;
      age = _other.age;
    }
    return select;
  }

  bool Combine(const Reservoir _other, float _random, float _target_pdf = 1.f) {
    return Resample(_other, _random, _target_pdf, _other.weight_sum * _other.M,
                    _other.M);
  }

  // pi and pi_sum in equation 6
  void FinializeRIS(float _normalized_numerator,
                    float _normalized_denominator) {
    float denominator = _normalized_denominator * target_pdf;
    weight_sum *= (denominator == 0) ? 0 : _normalized_numerator / denominator;
  }

  void StoreVisibility(const float3 _visibility, bool _b_discard_if_invisible) {
    packed_visibility =
        uint(saturate(_visibility.x) *
             s_packed_di_reservoir_visibility_channel_max) |
        (uint(saturate(_visibility.y) *
              s_packed_di_reservoir_visibility_channel_max)
         << s_packed_di_reservoir_visibility_channel_shift) |
        (uint(saturate(_visibility.z) *
              s_packed_di_reservoir_visibility_channel_max)
         << (s_packed_di_reservoir_visibility_channel_shift * 2));

    spatial_dist = int2(0, 0);
    age = 0;

    if (_b_discard_if_invisible && _visibility.x == 0 && _visibility.y == 0 &&
        _visibility.z == 0) {
      light_data = 0;
      weight_sum = 0;
    }
  }

  PackedReservoir Pack() {
    PackedReservoir packed;
    packed.light_data = light_data;
    packed.uv_data = uv_data;
    packed.target_pdf = target_pdf;
    packed.visibility =
        packed_visibility | (min(uint(M), s_packed_di_reservoir_max_M)
                             << s_packed_di_reservoir_M_shift);
    packed.distance_age =
        (uint(spatial_dist.x) & s_packed_di_reservoir_distance_mask)
            << s_packed_di_reservoir_distance_x_shift |
        (uint(spatial_dist.y) & s_packed_di_reservoir_distance_mask)
            << s_packed_di_reservoir_distance_y_shift |
        clamp(age, 0, s_packed_di_reservoir_max_age)
            << s_packed_di_reservoir_age_shift;
    packed.weight = weight_sum;
    return packed;
  }

  static Reservoir Unpack(PackedReservoir r) {
    Reservoir unpacked;
    unpacked.light_data = r.light_data;
    unpacked.uv_data = r.uv_data;
    unpacked.target_pdf = r.target_pdf;
    unpacked.packed_visibility = r.visibility;
    unpacked.M = (r.visibility >> s_packed_di_reservoir_M_shift) &
                 s_packed_di_reservoir_max_M;
    unpacked.spatial_dist =
        int2((r.distance_age >> s_packed_di_reservoir_distance_x_shift) &
                 s_packed_di_reservoir_distance_mask,
             (r.distance_age >> s_packed_di_reservoir_distance_y_shift) &
                 s_packed_di_reservoir_distance_mask);
    unpacked.age = (r.distance_age >> s_packed_di_reservoir_age_shift) &
                   s_packed_di_reservoir_max_age;
    unpacked.weight_sum = r.weight;

    if (isinf(unpacked.weight_sum) || isnan(unpacked.weight_sum))
      unpacked = Reservoir::EmptyReservoir();
    return unpacked;
  }
};

void StoreReservoir(Reservoir _res, ReservoirBufferParams _params,
                    uint2 _reservoir_pos, uint _array_idx) {

  uint idx = ReservoirPositionToIndex(_params, _reservoir_pos, _array_idx);
  DI_LIGHT_RESERVOIR_BUFFER[idx] = _res.Pack();
}

Reservoir LoadReservoir(ReservoirBufferParams _params, uint2 _reservoir_pos,
                        uint _array_idx) {
  uint idx = ReservoirPositionToIndex(_params, _reservoir_pos, _array_idx);
  return Reservoir::Unpack(DI_LIGHT_RESERVOIR_BUFFER[idx]);
}
// Reservoir UnpackReservoir(PackedReservoir r) {
//   Reservoir unpacked;
//   unpacked.light_data = r.light_data;
//   unpacked.uv_data = r.uv_data;
//   unpacked.target_pdf = r.target_pdf;
//   unpacked.packed_visibility = r.visibility;
//   unpacked.M = (r.visibility >> s_packed_di_reservoir_M_shift) &
//                s_packed_di_reservoir_max_M;
//   unpacked.spatial_dist =
//       int2((r.distance_age >> s_packed_di_reservoir_distance_x_shift) &
//                s_packed_di_reservoir_distance_mask,
//            (r.distance_age >> s_packed_di_reservoir_distance_y_shift) &
//                s_packed_di_reservoir_distance_mask);
//   unpacked.age = (r.distance_age >> s_packed_di_reservoir_age_shift) &
//                  s_packed_di_reservoir_max_age;
//   unpacked.weight_sum = r.weight;

//   if (isinf(unpacked.weight_sum) || isnan(unpacked.weight_sum))
//     unpacked = Reservoir::EmptyReservoir();
//   return unpacked;
// }
} // namespace DI

} // namespace Moer

#endif // MOER_FRAMEWORK_DI_RESERVOIRS_HLSLI