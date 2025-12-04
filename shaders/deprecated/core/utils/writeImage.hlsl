#include "features/3dgs/3dgs.hlsl"

[[vk::binding(0, 0)]] RWTexture2D<float4> output_image : register(u0);

struct params {
    uint width;
    uint height;
};

[[vk::push_constant]] ConstantBuffer<params> params : register(b0);

#define TILE_WIDTH  32
#define TILE_HEIGHT 32

[numthreads(TILE_WIDTH, TILE_HEIGHT, 1)] void main(uint3 dispatchThreadId
                                                   : SV_GroupThreadID, uint3 group_id
                                                   : SV_GroupID) {
    uint width  = params.width;
    uint height = params.height;

    uint tileX  = group_id.x;
    uint tileY  = group_id.y;
    uint localX = dispatchThreadId.x;
    uint localY = dispatchThreadId.y;

    uint2 curr_uv = uint2(tileX * TILE_WIDTH + localX, tileY * TILE_HEIGHT + localY);
//printf("tile %d %d %d %d uv %d %d\n", tileX, tileY, localX, localY, curr_uv.x, curr_uv.y);
    if (curr_uv.x >= width || curr_uv.y >= height) {
        return;
    }

    output_image[curr_uv] = float4(1.f,0.f,0.f, 1.0f);
	return ;

   
}
