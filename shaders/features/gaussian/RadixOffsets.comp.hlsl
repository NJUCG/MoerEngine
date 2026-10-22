#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint> histogram;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> offsets;
groupshared uint bin_totals[256];
[numthreads(256,1,1)]
void main(uint3 local_id : SV_GroupThreadID) {
    if (status[0].y != 0) return;
    uint bin_id = local_id.x, group_count = (status[0].x + 255) / 256;
    uint count = 0;
    for (uint group_id = 0; group_id < group_count; ++group_id) {
        uint slot = group_id * 256 + bin_id;
        offsets[slot] = count;
        count += histogram[slot];
    }
    bin_totals[bin_id] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1; step < 256; step <<= 1) {
        uint addend = bin_id >= step ? bin_totals[bin_id - step] : 0;
        GroupMemoryBarrierWithGroupSync();
        bin_totals[bin_id] += addend;
        GroupMemoryBarrierWithGroupSync();
    }
    uint base = bin_id == 0 ? 0 : bin_totals[bin_id - 1];
    for (uint group_id = 0; group_id < group_count; ++group_id)
        offsets[group_id * 256 + bin_id] += base;
}
