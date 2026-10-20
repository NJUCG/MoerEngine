#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<Moer::GProjectedGaussian> projected;
[[vk::binding(1, 0)]] StructuredBuffer<uint> prefix;
[[vk::binding(2, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint4> pairs;
[numthreads(256,1,1)]
void main(uint3 thread_id : SV_DispatchThreadID) {
    uint value_id = thread_id.x;
    if (value_id >= param.info.x || status[0].y != 0) return;
    uint offset = value_id == 0 ? 0 : prefix[value_id - 1];
    Moer::GProjectedGaussian gaussian = projected[value_id];
    for (uint tile_y = gaussian.tile_rect.y; tile_y < gaussian.tile_rect.w; ++tile_y)
        for (uint tile_x = gaussian.tile_rect.x; tile_x < gaussian.tile_rect.z; ++tile_x)
            pairs[offset++] = uint4(tile_x + tile_y * param.info.z, asuint(gaussian.center_depth.z), value_id, 0);
}
