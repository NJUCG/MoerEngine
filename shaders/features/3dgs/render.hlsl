#include "features/3dgs/3dgs.hlsl"

[[vk::binding(0, 0)]] StructuredBuffer<VertexAttribute> vertex_attribute_buffer : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint>            boundaries : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>            sorted_vertices : register(t2, space0);

[[vk::binding(0, 1)]] RWTexture2D<float4> output_image : register(u0);

struct params {
    uint width;
    uint height;
};

[[vk::push_constant]] ConstantBuffer<params> params : register(b0);

#define TILE_WIDTH  16
#define TILE_HEIGHT 16

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
	//printf("test");
//printf("tile %d %d %d %d uv %d %d\n", tileX, tileY, localX, localY, curr_uv.x, curr_uv.y);
    if (curr_uv.x >= width || curr_uv.y >= height) {
	//	printf("curr_uv.x curr_uv.y width height %d %d %d %d\n", curr_uv.x, curr_uv.y, width, height);
        return;
    }


    

    uint tiles_width = (width + TILE_WIDTH - 1) / TILE_WIDTH;

    uint start = boundaries[(tileX + tileY * tiles_width) * 2];
    uint end   = boundaries[(tileX + tileY * tiles_width) * 2 + 1];

    // if(start!=end){
    //     printf("start end %d %d\n", start, end);
    // }
    if(start >= end) {
		//output_image[curr_uv] = float4(0,1,0, 1.0f);
		//return ;    //    return;
    }
    //printf("start end %d %d\n", start, end);

    float  T = 1.0f;
    float3 c = float3(0.0f, 0.0f, 0.0f);

    for (uint i = start; i < end; i++) {
        uint   vertex_key = sorted_vertices[i];
        float2 uv         = vertex_attribute_buffer[vertex_key].uv;
        float2 distance   = uv - float2(curr_uv);
        float4 co         = vertex_attribute_buffer[vertex_key].conic_opacity;
        float  power      = -0.5f * (co.x * distance.x * distance.x + co.z * distance.y * distance.y) - co.y * distance.x * distance.y;


        if (power > 0.0f) {
            continue;
        }

        float alpha = min(0.99f, co.w * exp(power));

        if (alpha < 1.0f / 255.0f) {
            continue;
        }
  
        float test_T = T * (1 - alpha);
        if (test_T < 0.0001f) {
            break;
        }

        c += vertex_attribute_buffer[vertex_key].color_radii.xyz * alpha * T;

        T = test_T;
    }

  	output_image[curr_uv] = float4(c, 1.0f);
}
