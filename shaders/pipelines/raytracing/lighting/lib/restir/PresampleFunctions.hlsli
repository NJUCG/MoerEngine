
#ifndef MOER_DI_PRESAMPLE_FUNCTIONS_HLSLI
#define MOER_DI_PRESAMPLE_FUNCTIONS_HLSLI
#include <pipelines/raytracing/lighting/lib/restir/GridCommon.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/LightSelection.hlsli>
namespace Moer {

namespace SampleFunc {

void SamplePdfMip(inout RandomState _rng, Texture2D<float> _pdf_tex,
                  uint2 tex_size, out uint2 _pos, out float _pdf) {
  int last_mip = max(0, int(floor(log2(float(tex_size.x)))));
  _pos = uint2(0, 0);
  _pdf = 1.f;

  // sample from last mip to first mip
  for (int mip = last_mip; mip >= 0; mip--) {

    _pos <<= 1;

    float4 pdf_samples =
        float4(max(0.f, _pdf_tex.Load(int3(_pos.x, _pos.y, mip))),
               max(0.f, _pdf_tex.Load(int3(_pos.x + 1, _pos.y, mip))),
               max(0.f, _pdf_tex.Load(int3(_pos.x, _pos.y + 1, mip))),
               max(0.f, _pdf_tex.Load(int3(_pos.x + 1, _pos.y + 1, mip))));

    float pdf_sum =
        pdf_samples.x + pdf_samples.y + pdf_samples.z + pdf_samples.w;
    if (pdf_sum <= 0.f) {
      _pdf = 0.f;
      return;
    }

    pdf_samples /= pdf_sum;
    float rand = _rng.GetFloat();

    if (rand < pdf_samples.x) {
      _pdf *= pdf_samples.x;
    } else {
      rand -= pdf_samples.x;
      if (rand < pdf_samples.y) {
        _pos += int2(1, 0);
        _pdf *= pdf_samples.y;
      } else {
        rand -= pdf_samples.y;
        if (rand < pdf_samples.z) {
          _pos += int2(0, 1);
          _pdf *= pdf_samples.z;
        } else {
          _pos += int2(1, 1);
          _pdf *= pdf_samples.w;
        }
      }
    }
  }
}

void SampleLocalLights(inout RandomState _rng, Texture2D<float> _pdf_tex,
                       uint2 _tex_size, uint _tile_idx, uint _sample_in_tile,
                       Moer::DI::LightBufferRegion _local_light_region,
                       Moer::DI::RISBufferSegmentParams _ris_params) {
  uint2 tex_pos;
  float pdf;
  SamplePdfMip(_rng, _pdf_tex, _tex_size, tex_pos, pdf);
  uint light_idx = Math::ZCurveToLinearIndex(tex_pos);
  uint ris_idx = _ris_params.buffer_offset + _ris_params.tile_size * _tile_idx +
                 _sample_in_tile;
  float inv_pdf = 0.f;
  bool compact = false;

  light_idx += _local_light_region.first_light_idx;
  if (pdf > 0.f) {
    inv_pdf = 1.f / pdf;
    PolymorphicLightInfo light_info = LoadLightInfo(light_idx);
    compact = StoreCompactLightInfo(ris_idx, light_info);
  }

  if (compact) {
    light_idx |= s_di_light_compact_bit;
  }
  rw_ris_buffer[ris_idx] = uint2(light_idx, asuint(inv_pdf));
}

void SampleEnvMap(inout RandomState _rng, Texture2D<float> _pdf_tex,
                  uint2 _tex_size, uint _tile_idx, uint _sample_in_tile,
                  Moer::DI::RISBufferSegmentParams _ris_params) {

  uint2 tex_pos;
  float pdf;
  SampleFunc::SamplePdfMip(_rng, _pdf_tex, _tex_size, tex_pos, pdf);

  float2 sub_pixel_offset = _rng.GetFloat2();
  float2 uv = (float2(tex_pos) + sub_pixel_offset) / float2(_tex_size);

  uint packed_uv =
      uint(saturate(uv.x) * 0xffff) | (uint(saturate(uv.y) * 0xffff) << 16);
  float inv_pdf = pdf > 0.f ? 1.f / pdf : 0.f;
  uint ris_idx = _ris_params.buffer_offset + _ris_params.tile_size * _tile_idx +
                 _sample_in_tile;
  rw_ris_buffer[ris_idx] = uint2(packed_uv, asuint(inv_pdf));
}

void SampleLocalLightsForGrid(inout RandomState _rng,
                              inout RandomState _coherent_rng, uint _light_slot,
                              DI::LightBufferRegion _local_light_region,
                              DI::RISBufferSegmentParams _ris_params,
                              Grid::Params _grid_params) {
  uint ris_idx = _grid_params.common_params.ris_buffer_offset + _light_slot;
  if (_grid_params.common_params.num_build_samples == 0) {
    rw_ris_buffer[ris_idx] = uint2(0, 0);
    return;
  }

  uint light_in_ceil = _light_slot % _grid_params.common_params.lights_per_cell;
  uint cell_idx = _light_slot / _grid_params.common_params.lights_per_cell;

  float3 cell_center;
  float cell_radius;

  if (!Grid::WorldPosFromCellIdx(_grid_params, int(cell_idx), cell_center,
                                 cell_radius)) {
    rw_ris_buffer[ris_idx] = uint2(0, 0);
    return;
  }

  // apply jitter
  cell_radius *= (_grid_params.common_params.jitter + 1.f);

  PolymorphicLightInfo light_info = EmptyLightInfo();
  uint seleted = 0;
  float selected_target_pdf = 0.f;
  float weight_sum = 0.f;

  DI::LocalLightSelectionContext ctx;
  switch (_grid_params.common_params.local_light_sample_mode) {
  case s_di_local_light_sample_mode_power_ris:
    ctx = DI::LocalLightSelectionContext::CreatePowerRIS(_rng, _ris_params);
    break;
  default:
    ctx = DI::LocalLightSelectionContext::CreateUniform(_local_light_region);
    break;
  }

  for (uint i = 0; i < _grid_params.common_params.num_build_samples; i++) {

    PolymorphicLightInfo cur_light_info = EmptyLightInfo();
    uint rnd_light_idx = 0;
    float inv_pdf = 0.f;

    float rnd = _rng.GetFloat();

    ctx.SelectNext(_rng, cur_light_info, rnd_light_idx, inv_pdf);

    float target_pdf = PolymorphicLight::GetVolumeWeight(
        cur_light_info, cell_center, cell_radius);
    float ris_rnd = _rng.GetFloat();

    float ris_weight = target_pdf * inv_pdf;
    weight_sum += ris_weight;

    if (ris_rnd * weight_sum < ris_weight) {
      light_info = cur_light_info;
      seleted = rnd_light_idx;
      selected_target_pdf = target_pdf;
    }
  }

  // One-sample RIS estimator. Here p_hat is the unnormalized target, q is the
  // proposal PDF, and M is num_build_samples:
  //   w_i = p_hat(x_i) / q(x_i)
  //   I_hat_RIS = (sum_i(w_i) / M) * f(y) / p_hat(y)
  // The reservoir selects y proportional to w_i, so 1/M can be applied after
  // selection. `weight` stores sum_i(w_i) / (M * p_hat(y)) as the effective
  // inverse proposal weight consumed by later light sampling.
  float weight =
      (selected_target_pdf > 0.f)
          ? weight_sum /
                (float(_grid_params.common_params.num_build_samples) *
                 selected_target_pdf)
          : 0.f;
  bool compact = false;

  if (weight > 0.f) {
    compact = StoreCompactLightInfo(ris_idx, light_info);
  }

  if (compact) {
    seleted |= s_di_light_compact_bit;
  }
  rw_ris_buffer[ris_idx] = uint2(seleted, asuint(weight));
}

} // namespace SampleFunc
} // namespace Moer

#endif
