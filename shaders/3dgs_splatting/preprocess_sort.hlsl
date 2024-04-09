#include "3dgs.hlsl"
[[vk::binding(0, 0)]] StructuredBuffer<VertexAttribute> attr : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint>            prefix_sum : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint64_t>          keys : register(u2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>          payloads : register(u3, space0);


struct Params {
    uint tileX;
};

[[vk::push_constant]] ConstantBuffer<Params> params : register(b0);

[numthreads(256, 1, 1)] void main(uint3 dispatchThreadId
                                  : SV_DispatchThreadID) {
    uint tileX = params.tileX;

    uint index = dispatchThreadId.x;
    uint length, stride;
    prefix_sum.GetDimensions(length, stride);
    if (index >= length) {
        return;
    }

    if (attr[index].color_radii.w == 0) {
        return;
    }

    uint ind = index == 0 ? 0 : prefix_sum[index - 1];
 	//printf("index length ind %d %d %d\n", index, length, ind);

    for (uint i = attr[index].aabb.x; i < attr[index].aabb.z; i++) {
        for (uint j = attr[index].aabb.y; j < attr[index].aabb.w; j++) {
            uint tileIndex = i + j * tileX;
        
            uint depthBits = asuint(attr[index].depth);
            uint64_t k         = (uint64_t(tileIndex) << 32) | uint(depthBits);

            keys[ind]      = k;
			//uint64_t key = 1<<32;
			//printf("key %llu %llu\n",k, key);
      
		//	printf("tileindex %d key %llu %llu\n",tileIndex, k,tileIndex << 32);
            payloads[ind]  = index;
            ind++;
        }
    }
}
