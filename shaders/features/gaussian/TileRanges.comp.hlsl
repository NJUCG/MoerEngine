#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint4> pairs;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> ranges;
[numthreads(256,1,1)]
void main(uint3 thread_id : SV_DispatchThreadID) {
    uint value_id = thread_id.x, count = status[0].x;
    if (status[0].y != 0 || value_id >= count) return;
    uint tile_id = pairs[value_id].x;
    if (value_id == 0 || pairs[value_id - 1].x != tile_id) ranges[tile_id * 2] = value_id;
    if (value_id + 1 == count || pairs[value_id + 1].x != tile_id) ranges[tile_id * 2 + 1] = value_id + 1;
}
