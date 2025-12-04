#ifndef MOER_DI_RIS_COMMON_HLSLI

#define MOER_DI_RIS_COMMON_HLSLI

namespace Moer {
namespace DI {

struct RISTileInfo {
  uint tile_offset;
  uint tile_size; // as const param
};

// Select Presampled RIS light data from ris buffer
void RandomlySelectLightDataFromRISTile(inout RandomState _rng,
                                        RISTileInfo _tile_info,
                                        out uint2 _tile_data,
                                        out uint _ris_buf_idx) {
  float rng = _rng.GetFloat();
  uint ris_sample =
      min(uint(floor(rng * _tile_info.tile_size)), _tile_info.tile_size - 1);
  _ris_buf_idx = _tile_info.tile_offset + ris_sample;
  _tile_data = rw_ris_buffer[_ris_buf_idx];
}

RISTileInfo RandomlySelectRISTile(inout RandomState _rng,
                                  RISBufferSegmentParams _ris_params) {
  RISTileInfo result;
  float rng = _rng.GetFloat();
  uint tile_idx =
      min(uint(floor(rng * _ris_params.tile_cnt)), _ris_params.tile_cnt - 1);
  result.tile_offset =
      tile_idx * _ris_params.tile_size + _ris_params.buffer_offset;
  result.tile_size = _ris_params.tile_size;
  return result;
}

} // namespace DI
} // namespace Moer
#endif