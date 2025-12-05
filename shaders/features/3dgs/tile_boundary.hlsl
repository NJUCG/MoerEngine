
[[vk::binding(0, 0)]]StructuredBuffer<uint64_t>   sort_list : register(t0, space0);
[[vk::binding(1, 0)]]RWStructuredBuffer<uint> sort_out : register(u0, space0);

struct Params {
    uint num_instances;
};

[[vk::push_constant]] ConstantBuffer<Params> params;


#define LOCAL_SIZE_X 256
#define LOCAL_SIZE_Y 1
#define LOCAL_SIZE_Z 1

[numthreads(LOCAL_SIZE_X, LOCAL_SIZE_Y, LOCAL_SIZE_Z)] void main(uint3 dispatchThreadId
                                                                 : SV_DispatchThreadID) {
    uint numInstances = params.num_instances;
    uint index        = dispatchThreadId.x;
    if (index >= numInstances) {
        return;
    }

    uint key = uint(sort_list[index] >> 32);
    if (index == 0) {
        sort_out[key * 2] = index;
    } else {
        uint prevKey = uint(sort_list[index - 1] >> 32);
        if (prevKey > key) {
       // printf("prevKey > key: %d > %d\n", prevKey, key);
        }
        if (key != prevKey) {
	//	if(key<1000)
          //  printf("key prevkey index %d %d %d\n", key, prevKey, index);
            sort_out[key * 2]         = index;
            sort_out[prevKey * 2 + 1] = index;
        }
    }
    if (index == numInstances - 1) {
        sort_out[key * 2 + 1] = numInstances;
    }
}
