#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint4> pairs;
[[vk::binding(1, 0)]] StructuredBuffer<uint4> status;
[[vk::binding(2, 0)]] StructuredBuffer<uint> offsets;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint4> sorted_pairs;
// Portable stable rank within a 256-element block. No subgroup width or 64-bit atomics required.
groupshared uint bin_masks[256 * 8];
[numthreads(256,1,1)]
void main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
    uint count = status[0].x, lane_id = local_id.x;
    if (status[0].y != 0 || group_id.x >= (count + 255) / 256) return;
    for (uint word_id = lane_id; word_id < 256 * 8; word_id += 256) bin_masks[word_id] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint value_id = group_id.x * 256 + lane_id;
    uint4 pair = 0;
    uint bin_id = 0, lane_word = lane_id / 32, lane_bit = lane_id & 31;
    if (value_id < count) {
        pair = pairs[value_id];
        uint key = param.job.x < 4 ? pair.y : pair.x;
        bin_id = (key >> ((param.job.x & 3) * 8)) & 255;
        InterlockedOr(bin_masks[bin_id * 8 + lane_word], 1u << lane_bit);
    }
    GroupMemoryBarrierWithGroupSync();
    if (value_id < count) {
        uint rank = 0;
        [unroll] for (uint word_id = 0; word_id < 8; ++word_id) {
            uint mask = bin_masks[bin_id * 8 + word_id];
            if (word_id < lane_word) rank += countbits(mask);
            else if (word_id == lane_word) rank += countbits(mask & ((1u << lane_bit) - 1));
        }
        sorted_pairs[offsets[group_id.x * 256 + bin_id] + rank] = pair;
    }
}
