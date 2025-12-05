#ifndef MOER_DI_LIGHT_SELECTION_HLSLI
#define MOER_DI_LIGHT_SELECTION_HLSLI

#include <lighting/lib/restir/RISCommon.hlsli>
namespace Moer {

namespace DI {

void GetLightInfoFromRisData(uint2 _tile_data, uint _ris_buf_idx,
                             out PolymorphicLightInfo _light_info,
                             out uint _light_idx, out float _inv_pdf) {
  _light_idx = _tile_data.x & s_di_light_idx_mask;
  _inv_pdf = asfloat(_tile_data.y);

  if (_tile_data.x & s_di_light_compact_bit) {
    _light_info = LoadCompactLightInfo(_ris_buf_idx);
  } else {
    _light_info = LoadLightInfo(_light_idx);
  }
}

void RandomlySelectLightFromRISTile(inout RandomState _rng,
                                    RISTileInfo _tile_info,
                                    out PolymorphicLightInfo _light_info,
                                    out uint _light_idx, out float _inv_pdf) {
  uint2 tile_data;
  uint ris_buf_idx;
  RandomlySelectLightDataFromRISTile(_rng, _tile_info, tile_data, ris_buf_idx);
  GetLightInfoFromRisData(tile_data, ris_buf_idx, _light_info, _light_idx,
                          _inv_pdf);
}

void RandomlySelectLightDataUniformly(inout RandomState _rng,
                                      LightBufferRegion _region,
                                      out PolymorphicLightInfo _light_info,
                                      out uint _light_idx, out float _inv_pdf) {
  float rng = _rng.GetFloat();
  _inv_pdf = float(_region.light_cnt);
  _light_idx =
      min(uint(floor(rng * _region.light_cnt)), _region.light_cnt - 1) +
      _region.first_light_idx;
  _light_info = LoadLightInfo(_light_idx);
}

struct LocalLightSelectionContext {
  uint sample_mode;
  RISTileInfo ris_tile_info;
  LightBufferRegion light_region;

  static LocalLightSelectionContext
  CreateUniform(LightBufferRegion _light_region) {
    LocalLightSelectionContext result;
    result.sample_mode = s_di_local_light_sample_mode_uniform;
    result.light_region = _light_region;
    return result;
  }

  static LocalLightSelectionContext CreatePowerRIS(RISTileInfo _ris_tile_info) {
    LocalLightSelectionContext result;
    result.sample_mode = s_di_local_light_sample_mode_power_ris;
    result.ris_tile_info = _ris_tile_info;
    return result;
  }

  static LocalLightSelectionContext
  CreatePowerRIS(inout RandomState _rng, RISBufferSegmentParams _ris_params) {
    LocalLightSelectionContext result;
    result.sample_mode = s_di_local_light_sample_mode_power_ris;
    result.ris_tile_info = RandomlySelectRISTile(_rng, _ris_params);
    return result;
  }

  void SelectNext(inout RandomState _rng, out PolymorphicLightInfo _light_info,
                  out uint _light_idx, out float _inv_pdf) {
    switch (sample_mode) {

    case s_di_local_light_sample_mode_power_ris:
      RandomlySelectLightFromRISTile(_rng, ris_tile_info, _light_info,
                                     _light_idx, _inv_pdf);
      break;

    default:
    case s_di_local_light_sample_mode_uniform:
      RandomlySelectLightDataUniformly(_rng, light_region, _light_info,
                                       _light_idx, _inv_pdf);
      break;
    }
  }
};

} // namespace DI
} // namespace Moer
#endif