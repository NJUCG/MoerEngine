#ifndef MOER_RESTIR_INITIAL_SAMPLE_FUNCTIONS_HLSLI
#define MOER_RESTIR_INITIAL_SAMPLE_FUNCTIONS_HLSLI

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
                              float _brdf_ray_tmin = 0.001f) {
    SampleConfigs result;
    result.num_local_light = _num_local_light;
    result.num_infinite_light = _num_infinite_light;
    result.num_envmap = _num_envmap;
    result.num_brdf = _num_brdf;
    result.num_mis = _num_local_light + _num_envmap + _num_brdf;
    result.local_light_mis_weight =
        float(_num_local_light) / float(result.num_mis);
    result.env_map_mis_weight =
        float(_num_infinite_light) / float(result.num_mis);
    result.brdf_mis_weight = float(_num_brdf) / float(result.num_mis);
    result.brdf_cutoff = _brdf_cutoff;
    result.brdf_ray_tmin = _brdf_ray_tmin;
    return result;
  }
};

Reservoir SampleEnvMap(inout RandomState _rng, inout RandomState _coherent_rng,
                       in Surface _surface, in SampleConfigs _sample_configs,
                       in RISBufferSegmentParams _ris_params,
                       in EnvLightParams _env_light_params out LightSample
                           _light_sample) {

  Reservoir res = Reservoir::EmptyReservoir();
  _light_sample = LightSample::EmptyLightSample();

  if (!_env_light_params.light_cnt)
    return res;

  if (_sample_configs.num_envmap == 0)
    return res;
}

Reservoir SampleSurface(inout RandomState _rng, inout RandomState _coherent_rng,
                        in Surface _surface,
                        in LightBufferParams _light_buffer_params,
                        in RISBufferSegmentParams _local_light_ris_params,
                        in RISBufferSegmentParams _env_light_ris_params,
                        in Grid::Params _grid_light_params, uint _sample_mode,
                        out LightSample _light_sample) {
  _light_sample = LightSample::EmptyLightSample();

  Reservoir local_res;
  LightSample local_light_sample = LightSample::EmptyLightSample();

  local_res =
}
} // namespace DI
} // namespace Moer

#endif