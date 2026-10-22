#include "features/gaussian/GaussianMath.hlsli"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint> input_values;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> prefix;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> block_sums;
groupshared uint values[256];
[numthreads(256,1,1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint lane_id = local_id.x, value_id = group_id.x * 256 + lane_id;
    values[lane_id] = value_id < param.job.x ? input_values[value_id] : 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint offset = 1; offset < 256; offset <<= 1) {
        uint addend = lane_id >= offset ? values[lane_id - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        values[lane_id] = GaussianSaturatedAdd(values[lane_id], addend, param.info.y);
        GroupMemoryBarrierWithGroupSync();
    }
    if (value_id < param.job.x) prefix[value_id] = values[lane_id];
    if (lane_id == 255) block_sums[group_id.x] = values[255];
}
