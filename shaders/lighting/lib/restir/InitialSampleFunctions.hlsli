#ifndef MOER_RESTIR_INITIAL_SAMPLE_FUNCTIONS_HLSLI
#define MOER_RESTIR_INITIAL_SAMPLE_FUNCTIONS_HLSLI
#include <framework/DI/Reservoirs.hlsli>

#include <framework/DI/GridCommon.hlsli>
#include <framework/DI/RISCommon.hlsli>

#include <framework/DI/LightSelection.hlsli>

namespace Moer {
namespace DI {
struct SampleConfigs {

  // Number of samples for each type of light
  uint num_local_light;
  uint num_infinite_light;
  uint num_envmap;
  uint num_brdf;

  uint num_mis;
  float local_light_mis_weight;
  float env_map_mis_weight;
  float brdf_mis_weight;

  float brdf_cutoff;
  float brdf_ray_tmin;

  static SampleConfigs Create(uint _num_local_light, uint _num_infinite_light,
                              uint _num_envmap, uint _num_brdf,
                              float _brdf_cutoff = 0.f,
                              float _brdf_ray_tmin = 0.1f) {
    SampleConfigs result;
    result.num_local_light = _num_local_light;
    result.num_infinite_light = _num_infinite_light;
    result.num_envmap = _num_envmap;
    result.num_brdf = _num_brdf;
    result.num_mis = _num_local_light + _num_envmap + _num_brdf;
    result.local_light_mis_weight =
        float(_num_local_light) / float(result.num_mis);
    result.env_map_mis_weight = float(_num_envmap) / float(result.num_mis);
    result.brdf_mis_weight = float(_num_brdf) / float(result.num_mis);
    result.brdf_cutoff = _brdf_cutoff;
    result.brdf_ray_tmin = _brdf_ray_tmin;
    return result;
  }
};

float HeuresticMaxDistanceFromBrdfPdf(float _brdf_cutoff, float _brdf_pdf) {
  // float32 max
  const float s_ray_tmax = 1e30f;
  return _brdf_cutoff > 0.f ? sqrt((rcp(_brdf_cutoff) - 1.f) * _brdf_pdf)
                            : s_ray_tmax;
}

float ComputeLightBrdfMisWeight(Surface _surface, LightSample _light_sample,
                                float _light_selection_pdf,
                                float _light_mis_weight, bool _b_env_map,
                                SampleConfigs _configs) {

  float light_solid_angle_pdf = _light_sample.SolidAnglePdf();

  bool b_non_brdf = _configs.num_brdf == 0 || light_solid_angle_pdf <= 0.f ||
                    isinf(light_solid_angle_pdf) ||
                    isnan(light_solid_angle_pdf);
  if (b_non_brdf)
    return _light_mis_weight * _light_selection_pdf;

  float3 light_dir;
  float light_dist;
  _surface.GetLightDirAndDist(_light_sample, light_dir, light_dist);

  float brdf_pdf = _surface.GetBrdfPdf(light_dir);

  float max_brdf_dist =
      HeuresticMaxDistanceFromBrdfPdf(_configs.brdf_cutoff, brdf_pdf);
  if (!_b_env_map && light_dist > max_brdf_dist)
    brdf_pdf = 0.f;

  float src_pdf_wrt_solid_angle = _light_selection_pdf * light_solid_angle_pdf;

  float mis_weight_solid_angle = src_pdf_wrt_solid_angle * _light_mis_weight +
                                 brdf_pdf * _configs.brdf_mis_weight;

  return mis_weight_solid_angle / light_solid_angle_pdf;
}

float2 RandomlySelectLocalLightUV(inout RandomState _rng) {
  float2 rnd = _rng.GetFloat2();
  return rnd;
}

bool StreamLocalLightAtUV(inout RandomState _rng, SampleConfigs _configs,
                          Surface _surface, uint _light_idx, float2 _uv,
                          float _inv_src_pdf, PolymorphicLightInfo _light_info,
                          inout Reservoir _reservoir,
                          inout LightSample _light_sample) {

  LightSample candidate = _surface.SamplePolymorphicLight(_light_info, _uv);
  float mis_src_pdf = ComputeLightBrdfMisWeight(
      _surface, candidate, 1.f / _inv_src_pdf, _configs.local_light_mis_weight,
      false, _configs);
  float target_pdf = _surface.GetLightSampleTargetPdf(candidate);
  // printf("ris weight %f\n", 1.f / mis_src_pdf * target_pdf);
  float rnd = _rng.GetFloat();

  if (mis_src_pdf == 0.f) {
    return false;
  }
  bool selected = _reservoir.StreamSample(_light_idx, _uv, rnd, target_pdf,
                                          1.f / mis_src_pdf);
  if (selected) {
    _light_sample = candidate;
  }

  return true;
}

//////////////////////////////////////////////////////////////////////////
// Light Grid
//////////////////////////////////////////////////////////////////////////

int GetLightGridCellIdx(inout RandomState _rng, in Grid::Params _params,
                        Surface _surface) {

  float3 world_pos = _surface.GetWorldPos();

  float3 cell_jitter =
      float3(_rng.GetFloat(), _rng.GetFloat(), _rng.GetFloat());
  cell_jitter -= 0.5f;

  float jitter_scale = Moer::Grid::GetJitterScale(_params);
  world_pos += cell_jitter * jitter_scale;

  return Moer::Grid::CellIdxFromWorldPos(_params, world_pos);
}

RISTileInfo GetLocalLightGridTileInfo(int _cell_idx,
                                      Moer::Grid::CommonParams _params) {

  RISTileInfo result;
  uint cell_offset = uint(_cell_idx) * _params.lights_per_cell;
  result.tile_offset = cell_offset + _params.ris_buffer_offset;
  result.tile_size = _params.lights_per_cell;
  return result;
}

LocalLightSelectionContext CreateLocalLightGridSelectionContextForGrid(
    inout RandomState _rng, Moer::Grid::Params _params,
    Moer::DI::RISBufferSegmentParams _local_light_ris_params,
    LightBufferRegion _local_light_region, Surface _surface) {
  int cell_idx = GetLightGridCellIdx(_rng, _params, _surface);
  if (cell_idx >= 0) {
    RISTileInfo tile_info =
        GetLocalLightGridTileInfo(cell_idx, _params.common_params);
    return LocalLightSelectionContext::CreatePowerRIS(tile_info);
  } else if (_params.common_params.local_light_sampling_fallback_mode ==
             s_di_local_light_sample_mode_power_ris) {
    return LocalLightSelectionContext::CreatePowerRIS(_rng,
                                                      _local_light_ris_params);
  }
  return LocalLightSelectionContext::CreateUniform(_local_light_region);
}

//////////////////////////////////////////////////////////////////////////
// Local light selection
//////////////////////////////////////////////////////////////////////////

LocalLightSelectionContext InitializeLocalLightSelectionContext(
    inout RandomState _rng, uint _sample_mode,
    LightBufferRegion _local_light_region,
    RISBufferSegmentParams _local_light_ris_params, Grid::Params _grid_params,
    Surface _surface) {

  LocalLightSelectionContext ctx;
  if (_sample_mode == s_di_local_light_sample_mode_grid) {
    ctx = CreateLocalLightGridSelectionContextForGrid(
        _rng, _grid_params, _local_light_ris_params, _local_light_region,
        _surface);
  } else if (_sample_mode == s_di_local_light_sample_mode_power_ris) {
    ctx = LocalLightSelectionContext::CreatePowerRIS(_rng,
                                                     _local_light_ris_params);
  } else {
    ctx = LocalLightSelectionContext::CreateUniform(_local_light_region);
  }
  return ctx;
}

Reservoir SampleLocalLights(inout RandomState _rng,
                            inout RandomState _coherent_rng,
                            in SampleConfigs _sample_configs,
                            in Surface _surface, uint _sample_mode,
                            in LightBufferParams _light_buffer_params,
                            in RISBufferSegmentParams _local_light_ris_params,
                            in Grid::Params _grid_light_params,
                            out LightSample _light_sample) {
  _light_sample = LightSample::EmptyLightSample();
  Reservoir res = Reservoir::EmptyReservoir();

  if (_sample_configs.num_local_light == 0 ||
      _light_buffer_params.local_light_region.light_cnt == 0) {
    return res;
  }

  LocalLightSelectionContext ctx = InitializeLocalLightSelectionContext(
      _rng, _sample_mode, _light_buffer_params.local_light_region,
      _local_light_ris_params, _grid_light_params, _surface);

  for (uint i = 0; i < _sample_configs.num_local_light; i++) {
    PolymorphicLightInfo light_info;
    uint light_idx = 0;
    float inv_pdf = 0.f;

    ctx.SelectNext(_rng, light_info, light_idx, inv_pdf);
    float2 uv = _rng.GetFloat2();
    if (StreamLocalLightAtUV(_rng, _sample_configs, _surface, light_idx, uv,
                             inv_pdf, light_info, res, _light_sample)) {
      continue;
    }
  }

  res.FinalizeRIS(1.f, _sample_configs.num_mis);
  res.M = 1.f;

  return res;
}

//////////////////////////////////////////////////////////////////////////
// Infinite light selection
//////////////////////////////////////////////////////////////////////////

void StreamInfiniteLightAtUV(inout RandomState _rng, Surface _surface,
                             PolymorphicLightInfo _light_info, uint _light_idx,
                             float2 _uv, float _inv_src_pdf,
                             inout Reservoir _reservoir,
                             inout LightSample _light_sample) {
  LightSample candidate = _surface.SamplePolymorphicLight(_light_info, _uv);
  float target_pdf = _surface.GetLightSampleTargetPdf(candidate);
  float rnd = _rng.GetFloat();

  bool selected =
      _reservoir.StreamSample(_light_idx, _uv, rnd, target_pdf, _inv_src_pdf);
  if (selected) {
    _light_sample = candidate;
  }
  // printf("infinite light res weight_sum: %f\n", _reservoir.weight_sum);
}

Reservoir SampleInfiniteLights(inout RandomState _rng, Surface _surface,
                               uint _num_infinite_light,
                               LightBufferRegion _infinite_light_region,
                               inout LightSample _light_sample) {

  Reservoir res = Reservoir::EmptyReservoir();
  _light_sample = LightSample::EmptyLightSample();

  if (_num_infinite_light == 0 || _infinite_light_region.light_cnt == 0)
    return res;

  for (uint i = 0; i < _num_infinite_light; i++) {
    PolymorphicLightInfo light_info;
    uint light_idx = 0;
    float inv_pdf = 0.f;

    RandomlySelectLightDataUniformly(_rng, _infinite_light_region, light_info,
                                     light_idx, inv_pdf);
    float2 uv = _rng.GetFloat2();
    StreamInfiniteLightAtUV(_rng, _surface, light_info, light_idx, uv, inv_pdf,
                            res, _light_sample);
  }

  res.FinalizeRIS(1.f, res.M);
  res.M = 1.f;

  return res;
}

//////////////////////////////////////////////////////////////////////////
// Envmap selection
//////////////////////////////////////////////////////////////////////////

void UnpackEnvLightDataFromRISData(uint2 _data, out float2 _uv,
                                   out float _inv_src_pdf) {
  uint pack_uv = _data.x;
  _inv_src_pdf = asfloat(_data.y);
  _uv = float2((pack_uv & 0xffff), (pack_uv >> 16)) / float(0xffff);
}

void RandomlySelectEnvLightFromRISTile(inout RandomState _rng,
                                       RISTileInfo _tile_info, out float2 _uv,
                                       out float _inv_src_pdf) {
  uint2 data;
  uint ris_buf_idx;
  RandomlySelectLightDataFromRISTile(_rng, _tile_info, data, ris_buf_idx);
  UnpackEnvLightDataFromRISData(data, _uv, _inv_src_pdf);
}

void StreamEnvLightAtUV(inout RandomState _rng, SampleConfigs _configs,
                        Surface _surface, PolymorphicLightInfo _light_info,
                        uint _light_idx, float2 _uv, float _inv_src_pdf,
                        inout Reservoir _reservoir,
                        inout LightSample _light_sample) {

  LightSample candidate = _surface.SamplePolymorphicLight(_light_info, _uv);
  float target_pdf = _surface.GetLightSampleTargetPdf(candidate);
  float rnd = _rng.GetFloat();
  float mis_src_pdf =
      ComputeLightBrdfMisWeight(_surface, candidate, 1.f / _inv_src_pdf,
                                _configs.env_map_mis_weight, true, _configs);

  bool selected = _reservoir.StreamSample(_light_idx, _uv, rnd, target_pdf,
                                          1.f / mis_src_pdf);
  if (selected) {
    _light_sample = candidate;
  }
}

Reservoir SampleEnvMap(inout RandomState _rng, inout RandomState _coherent_rng,
                       in SampleConfigs _sample_configs, in Surface _surface,
                       in RISBufferSegmentParams _env_ris_params,
                       in EnvLightParams _env_light_params,
                       out LightSample _light_sample) {

  Reservoir res = Reservoir::EmptyReservoir();
  _light_sample = LightSample::EmptyLightSample();

  if (_env_light_params.light_cnt == 0 || _sample_configs.num_envmap == 0)
    return res;

  RISTileInfo tile_info = RandomlySelectRISTile(_rng, _env_ris_params);
  PolymorphicLightInfo light_info = LoadLightInfo(_env_light_params.light_idx);
  for (uint i = 0; i < _sample_configs.num_envmap; i++) {
    uint env_light_idx = 0;
    float2 uv;
    float inv_src_pdf = 0.f;

    RandomlySelectEnvLightFromRISTile(_rng, tile_info, uv, inv_src_pdf);
    StreamEnvLightAtUV(_rng, _sample_configs, _surface, light_info,
                       _env_light_params.light_idx, uv, inv_src_pdf, res,
                       _light_sample);
  }

  res.FinalizeRIS(1.f, _sample_configs.num_mis);
  res.M = 1.f;

  return res;
}

//////////////////////////////////////////////////////////////////////////
// BRDF selection
//////////////////////////////////////////////////////////////////////////

Reservoir SampleBrdf(inout RandomState _rng, SampleConfigs _sample_configs,
                     LightBufferParams _light_buffer_params, Surface _surface,
                     out LightSample _light_sample) {

  Reservoir res = Reservoir::EmptyReservoir();
  for (uint i = 0; i < _sample_configs.num_brdf; i++) {
    float src_pdf = 0.f;
    float3 dir;
    uint light_idx = s_invalid_light_idx;
    float2 rnd = float2(0.f, 0.f);
    LightSample candidate = LightSample::EmptyLightSample();

    if (_surface.GetBrdfSample(dir, _rng)) {
      float brdf_pdf = _surface.GetBrdfPdf(dir);
      float max_brdf_dist = HeuresticMaxDistanceFromBrdfPdf(
          _sample_configs.brdf_cutoff, brdf_pdf);
      bool b_hit = RaytraceLocalLightVisibility(_surface.GetWorldPos(), dir,
                                                _sample_configs.brdf_ray_tmin,
                                                max_brdf_dist, light_idx, rnd);

      if (light_idx != s_invalid_light_idx) {
        PolymorphicLightInfo light_info = LoadLightInfo(light_idx);
        candidate = _surface.SamplePolymorphicLight(light_info, rnd);
        if (_sample_configs.brdf_cutoff > 0.f) {
          float3 light_dir;
          float light_dist;
          _surface.GetLightDirAndDist(candidate, light_dir, light_dist);
          // printf("%f dir dot light dir \n", dot(dir, light_dir));

          float brdf_pdf = _surface.GetBrdfPdf(light_dir);
          float max_dist = HeuresticMaxDistanceFromBrdfPdf(
              _sample_configs.brdf_cutoff, brdf_pdf);
          if (light_dist > max_dist) {
            light_idx = s_invalid_light_idx;
          }
        }

        if (light_idx != s_invalid_light_idx) {
          src_pdf = EvalLocalLightSrcPdf(light_idx); // local light selection
                                                     // pdf
        }
      } else if (!b_hit && (_light_buffer_params.env_light.light_cnt != 0)) {
        // Sample envmap
        light_idx = _light_buffer_params.env_light.light_idx;
        PolymorphicLightInfo light_info = LoadLightInfo(light_idx);
        rnd = GetEnvironmentMapUVFromDir(dir);
        candidate = _surface.SamplePolymorphicLight(light_info, rnd);
        src_pdf = EvalEnvMapPdf(dir);
      }
    }

    if (src_pdf == 0.f)
      continue;

    bool b_is_env = light_idx == _light_buffer_params.env_light.light_idx;
    float target_pdf = _surface.GetLightSampleTargetPdf(candidate);
    float mis_weight = ComputeLightBrdfMisWeight(
        _surface, candidate, src_pdf, _sample_configs.brdf_mis_weight, b_is_env,
        _sample_configs);

    float ris_rnd = _rng.GetFloat();
    if (res.StreamSample(light_idx, rnd, ris_rnd, target_pdf,
                         1.f / mis_weight)) {
      _light_sample = candidate;
    }
  }

  res.FinalizeRIS(1.f, _sample_configs.num_mis);
  res.M = 1.f;

  return res;
}

Reservoir
SampleLightsForSurface(inout RandomState _rng, inout RandomState _coherent_rng,
                       in SampleConfigs _sample_configs, in Surface _surface,
                       in LightBufferParams _light_buffer_params,
                       in RISBufferSegmentParams _local_light_ris_params,
                       in RISBufferSegmentParams _env_light_ris_params,
                       in Grid::Params _grid_params, uint _sample_mode,
                       out LightSample _light_sample) {
  _light_sample = LightSample::EmptyLightSample();

  Reservoir local_res;
  LightSample local_light_sample = LightSample::EmptyLightSample();

  // sample local lights
  local_res = SampleLocalLights(_rng, _coherent_rng, _sample_configs, _surface,
                                _sample_mode, _light_buffer_params,
                                _local_light_ris_params, _grid_params,
                                local_light_sample);

  // sample infinite lights
  LightSample infinite_sample = LightSample::EmptyLightSample();
  Reservoir infinite_res = SampleInfiniteLights(
      _rng, _surface, _sample_configs.num_infinite_light,
      _light_buffer_params.infinite_light_region, infinite_sample);

  // sample envmap
  LightSample env_sample = LightSample::EmptyLightSample();
  Reservoir env_res = SampleEnvMap(_rng, _coherent_rng, _sample_configs,
                                   _surface, _env_light_ris_params,
                                   _light_buffer_params.env_light, env_sample);
  // sample brdf
  LightSample brdf_sample = LightSample::EmptyLightSample();
  Reservoir brdf_res = SampleBrdf(_rng, _sample_configs, _light_buffer_params,
                                  _surface, brdf_sample);

  Reservoir final_res = Reservoir::EmptyReservoir();

  final_res.Combine(local_res, 0.5f, local_res.target_pdf);
  bool select_infinite =
      final_res.Combine(infinite_res, _rng.GetFloat(), infinite_res.target_pdf);
  bool select_env =
      final_res.Combine(env_res, _rng.GetFloat(), env_res.target_pdf);
  bool select_brdf =
      final_res.Combine(brdf_res, _rng.GetFloat(), brdf_res.target_pdf);

  final_res.FinalizeRIS(1.f, 1.f);
  final_res.M = 1.f;

  if (select_brdf) {
    _light_sample = brdf_sample;
  } else if (select_env) {
    _light_sample = env_sample;
  } else if (select_infinite) {
    _light_sample = infinite_sample;
  } else {
    _light_sample = local_light_sample;
  }

  return final_res;
}
} // namespace DI
} // namespace Moer

#endif