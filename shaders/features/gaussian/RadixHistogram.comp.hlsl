#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint4> pairs;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> histogram;
groupshared uint bins[256];
[numthreads(256,1,1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint count = status[0].x, lane_id = local_id.x;
    if (status[0].y != 0 || group_id.x >= (count + 255) / 256) return;
    bins[lane_id] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint value_id = group_id.x * 256 + lane_id;
    if (value_id < count) {
        uint4 pair = pairs[value_id];
        uint key = param.job.x < 4 ? pair.y : pair.x;
        InterlockedAdd(bins[(key >> ((param.job.x & 3) * 8)) & 255], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    histogram[group_id.x * 256 + lane_id] = bins[lane_id];
}
