struct Params {
    uint g_num_instances;
    uint g_shift;
    uint g_num_workgroups;
    uint g_num_blocks_per_workgroup;
};

typedef uint64_t key_t;

#define WORKGROUP_SIZE  256// assert WORKGROUP_SIZE >= RADIX_SORT_BINS
#define RADIX_SORT_BINS 256U



[[vk::binding(0, 0)]] StructuredBuffer<key_t>  g_elements_in : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>  g_histograms : register(u0, space0);


[[vk::push_constant]] ConstantBuffer<Params> params;

groupshared uint histogram[RADIX_SORT_BINS];


[numthreads(WORKGROUP_SIZE, 1, 1)]
void main(uint3 local_thread_id
          : SV_GroupThreadID, uint3 global_group_id
          : SV_GroupID) {
    uint lID = local_thread_id.x;
    uint wID = global_group_id.x;

    if (lID < RADIX_SORT_BINS) {
        histogram[lID] = 0;
    }

    uint g_num_elements             = params.g_num_instances;
    uint g_shift                    = params.g_shift;
    uint g_num_workgroups           = params.g_num_workgroups;
    uint g_num_blocks_per_workgroup = params.g_num_blocks_per_workgroup;

    GroupMemoryBarrierWithGroupSync();
    for (uint index = 0; index < g_num_blocks_per_workgroup; index++) {
        uint elementId = wID * g_num_blocks_per_workgroup * WORKGROUP_SIZE + index * WORKGROUP_SIZE + lID;
        if (elementId < g_num_elements) {
            // determine the bin
            uint shift_bit = uint(g_elements_in[elementId] >> g_shift);
            const uint bin = shift_bit & (RADIX_SORT_BINS - 1);

         //   const uint bin = uint(g_elements_in[elementId] >> g_shift) & (RADIX_SORT_BINS - 1);
			if(g_elements_in[elementId] == 0 && g_shift == 8){
			//	printf("elementId: %d bin %d %d %llu\n", elementId, bin, g_shift, g_elements_in[elementId]);
			}
            // increment the histogram
            InterlockedAdd(histogram[bin], 1U);
        }
    }
    GroupMemoryBarrierWithGroupSync();

	uint idx = RADIX_SORT_BINS * wID + lID;
	
    g_histograms[RADIX_SORT_BINS * wID + lID] = histogram[lID];



}