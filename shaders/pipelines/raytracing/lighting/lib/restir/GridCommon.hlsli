#ifndef MOER_DI_GRID_COMMON_HLSLI
#define MOER_DI_GRID_COMMON_HLSLI
#include <core/math/STL.hlsli>
namespace Moer {
namespace Grid {

float GetJitterScale(Params _params) {
  return _params.common_params.jitter * _params.common_params.cell_size;
}

int CellIdxFromWorldPos(Params _params, float3 _world_pos) {
  const float3 grid_center =
      float3(_params.common_params.center_x, _params.common_params.center_y,
             _params.common_params.center_z);
  const int3 grid_cell_cnt =
      int3(_params.grid_params.cell_x, _params.grid_params.cell_y,
           _params.grid_params.cell_z);
  //    printf("grid_center %f %f %f\n", grid_center.x, grid_center.y,
  //    grid_center.z);
  const float3 grid_origin = grid_center - float3(grid_cell_cnt) *
                                               _params.common_params.cell_size *
                                               0.5f;

  int3 cell =
      int3(floor((_world_pos - grid_origin) / _params.common_params.cell_size));

  if (cell.x < 0 || cell.x >= grid_cell_cnt.x || cell.y < 0 ||
      cell.y >= grid_cell_cnt.y || cell.z < 0 || cell.z >= grid_cell_cnt.z) {
    return -1;
  }
  return cell.z * grid_cell_cnt.x * grid_cell_cnt.y + cell.y * grid_cell_cnt.x +
         cell.x;
}

bool WorldPosFromCellIdx(Params _params, int _cell_idx, out float3 _world_pos,
                         out float _cell_radius) {
  const float3 grid_center =
      float3(_params.common_params.center_x, _params.common_params.center_y,
             _params.common_params.center_z);
  const int3 grid_cell_cnt =
      int3(_params.grid_params.cell_x, _params.grid_params.cell_y,
           _params.grid_params.cell_z);
  const float3 grid_origin = grid_center - float3(grid_cell_cnt) *
                                               _params.common_params.cell_size *
                                               0.5f;

  uint3 cell_pos;
  cell_pos.x = _cell_idx % grid_cell_cnt.x;
  cell_pos.y = (_cell_idx / grid_cell_cnt.x) % grid_cell_cnt.y;
  cell_pos.z = _cell_idx / (grid_cell_cnt.x * grid_cell_cnt.y);
  if (cell_pos.z >= grid_cell_cnt.z) {
    _world_pos = float3(0.f, 0.f, 0.f);
    _cell_radius = 0.f;
    return false;
  }
  const float3 ceil_size = _params.common_params.cell_size;
  _world_pos = grid_origin +
               float3(cell_pos) * _params.common_params.cell_size +
               _params.common_params.cell_size * 0.5f;
  _cell_radius = _params.common_params.cell_size * sqrt(3.f);

  return true;
}

float3 GetVisualizeGridColor(Params _params, float3 _pos) {
  int idx = CellIdxFromWorldPos(_params, _pos);
  uint hash = STL::Sequence::Hash(idx);
  // printf("idx %d hash %d\n", idx, hash);
  // r11g11b10
  float3 color =
      float3((hash & 0x7ff) / 2047.f, ((hash >> 11) & 0x7ff) / 2047.f,
             ((hash >> 22) & 0x3ff) / 1023.f);

  return idx < 0 ? float3(1.f, 1.f, 1.f) : color;
}
} // namespace Grid
} // namespace Moer

#endif // MOER_DI_GRID_COMMON_HLSLI