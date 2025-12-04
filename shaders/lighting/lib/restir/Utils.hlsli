#ifndef MOER_FRAMEWORK_DI_UTILS_HLSLI
#define MOER_FRAMEWORK_DI_UTILS_HLSLI
namespace Moer {

uint ReservoirPositionToIndex(DI::ReservoirBufferParams _params, uint2 _pos,
                              uint _array_idx) {
  uint2 block_idx = _pos / s_di_reservoir_block_size;
  uint2 block_pos = _pos % s_di_reservoir_block_size;

  return _array_idx * _params.block_array_pitch +
         block_idx.y * _params.block_row_pitch +
         block_idx.x * (s_di_reservoir_block_size * s_di_reservoir_block_size) +
         block_pos.y * s_di_reservoir_block_size + block_pos.x;
}
} // namespace Moer
#endif