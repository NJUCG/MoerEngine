struct Params {
    uint g_num_instances;
    uint g_shift;
    uint g_num_workgroups;
    uint g_num_blocks_per_workgroup;
};

typedef uint64_t key_t;

#define WORKGROUP_SIZE  256
#define RADIX_SORT_BINS 256U
#define SUBGROUP_SIZE   32
#define BITS            64

[[vk::binding(0, 0)]] StructuredBuffer<key_t>  g_elements_in : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<key_t> g_elements_out : register(u0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>   g_payload_in : register(t1, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_payload_out : register(u1, space0);
[[vk::binding(4, 0)]] StructuredBuffer<uint>   g_histograms : register(t2, space0);

[[vk::push_constant]] ConstantBuffer<Params> params;

groupshared uint sums[RADIX_SORT_BINS / SUBGROUP_SIZE];
groupshared uint global_offsets[RADIX_SORT_BINS]; 

struct BinFlags {
    uint flags1[WORKGROUP_SIZE / BITS];
    uint flags2[WORKGROUP_SIZE / BITS];
};

groupshared BinFlags bin_flags[RADIX_SORT_BINS];

[numthreads(256, 1, 1)] void main(uint3 group_id
                                  : SV_GroupIndex, uint3 dispatch_id
                                  : SV_GroupThreadID) {
    // uint block_id   = group_id.x * g_params.g_num_blocks_per_workgroup + dispatch_id.x;
    // uint element_id = block_id * 256 + dispatch_id.y;
    // if (element_id >= g_params.g_num_instances) {
    //     return;
    // }
    // key_t key     = elements_in[element_id];
    // uint  payload = payload_in[element_id];
    // uint  count   = 0;
    // [unroll] for (uint i = 0; i < 64; i++) {
    //     count += (key >> i) & 1;
    // }
    // elements_out[element_id] = count;
    // payload_out[element_id]  = payload;
    uint t = g_histograms[RADIX_SORT_BINS * group_id.x + dispatch_id.x] + params.g_shift;
    g_elements_out[RADIX_SORT_BINS * group_id.x + dispatch_id.x] = t + g_elements_in[RADIX_SORT_BINS * group_id.x + dispatch_id.x];
    g_payload_out[RADIX_SORT_BINS * group_id.x + dispatch_id.x]  = g_payload_in[RADIX_SORT_BINS * group_id.x + dispatch_id.x];
    

    uint lID  = dispatch_id.x;
    uint wID  = group_id.x;
    uint sID  = lID / SUBGROUP_SIZE;
    uint lsID = WaveGetLaneIndex();

    uint local_histogram = 0;
    uint prefix_sum      = 0;
    uint histogram_count = 0;

    uint g_num_workgroups           = params.g_num_workgroups;
    uint g_num_blocks_per_workgroup = params.g_num_blocks_per_workgroup;
    uint g_num_instances            = params.g_num_instances;
    uint g_shift                    = params.g_shift;
 //   GroupMemoryBarrierWithGroupSync();
    {
        // uint count = 0;
        // for (uint j = 0; j < g_num_workgroups; j++) {
        //      uint t    = g_histograms[RADIX_SORT_BINS * j + lID];
        //     local_histogram = (j == wID) ? count : local_histogram;
        //     count += t;
       // }
     //   histogram_count = count;
        // const int sum   = WaveActiveSum(histogram_count);
        // prefix_sum      = WavePrefixSum(histogram_count);
        // if (WaveIsFirstLane()) {
        //     sums[wID] = sum;
        // }
    }
  //  GroupMemoryBarrierWithGroupSync();

    // if (lID < RADIX_SORT_BINS) {
    //     const uint sums_prefix_sum  = WaveReadLaneAt(WavePrefixSum(sums[lsID]), sID);
    //     const uint global_histogram = sums_prefix_sum + prefix_sum;
    //     global_offsets[lID]         = global_histogram + local_histogram;
    // } else {
    //     // printf("lID %d\n", lID);
    // }
    //
    // const uint flags_bin = lID / BITS;
    //
    // //lid和bit对应的flag
    // const uint64_t flags_bit = 1UL << (lID % BITS);

    // for (uint index = 0; index < g_num_blocks_per_workgroup; index++) {
    //     uint elementId = wID * g_num_blocks_per_workgroup * WORKGROUP_SIZE + index * WORKGROUP_SIZE + lID;
    //     if (lID < RADIX_SORT_BINS) {
    //         for (int i = 0; i < WORKGROUP_SIZE / BITS; i++) {
    //             bin_flags[lID].flags1[i] = 0U;// init all bin flags to 0
    //             bin_flags[lID].flags2[i] = 0U;// init all bin flags to 0
    //         }
    //     }
    //     GroupMemoryBarrierWithGroupSync();
    //     key_t element_in = 0;
    //     uint  payload_in = 0;
    //     uint  binID      = 0;
    //     uint  binOffset  = 0;
    //     if (elementId < g_num_instances) {
    //         element_in = g_elements_in[elementId];
    //         payload_in = g_payload_in[elementId];
    //         binID      = uint(element_in >> g_shift) & uint(RADIX_SORT_BINS - 1);
    //         // offset for group
    //         binOffset = global_offsets[binID];
    //         // add bit to flag
    //         InterlockedAdd(bin_flags[binID].flags1[flags_bin], uint(flags_bit));
    //         InterlockedAdd(bin_flags[binID].flags2[flags_bin], uint(flags_bit >> 32));
    //     }
    //     GroupMemoryBarrierWithGroupSync();
    //
    //     if (elementId < g_num_instances) {
    //         // calculate output index of element
    //         uint prefix = 0;
    //         uint count  = 0;
    //         for (uint i = 0; i < WORKGROUP_SIZE / BITS; i++) {
    //
    //             const uint     flag1         = bin_flags[binID].flags1[i];
    //             const uint     flag2         = bin_flags[binID].flags2[i];
    //             const uint     full_count    = countbits(flag1) + countbits(flag2);
    //             const uint64_t f             = flags_bit - 1;
    //             const uint     partial_bits1 = flag1 & uint(f);
    //             const uint     partial_bits2 = flag2 & uint(f >> 32);
    //             const uint     partial_count = countbits(partial_bits1) + countbits(partial_bits2);
    //
    //             prefix += (i < flags_bin) ? full_count : 0U;
    //             prefix += (i == flags_bin) ? partial_count : 0U;
    //             count += full_count;
    //         }
    //         g_elements_out[binOffset + prefix] = element_in;
    //         g_payload_out[binOffset + prefix]  = payload_in;
    //         if (prefix == count - 1) {
    //             InterlockedAdd(global_offsets[binID], count);
    //         }
    //     }
    //
    //     GroupMemoryBarrierWithGroupSync();
    // }
}