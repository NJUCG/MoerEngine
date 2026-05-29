#ifndef HIZ_REVERSE_Z
#define HIZ_REVERSE_Z 1
#endif

#include "shared/raster/culling/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::HiZBuildParam> param;

[[vk::binding(0, 0)]] Texture2D<float> src_texture;
[[vk::binding(1, 0)]] RWTexture2D<float> dst_texture;

float reduce_depth(float4 depth) {
#if HIZ_REVERSE_Z
    return max(max(depth.x, depth.y), max(depth.z, depth.w));
#else
    return min(min(depth.x, depth.y), min(depth.z, depth.w));
#endif
}

float load_src_depth(int2 coord) {
    int2 max_coord = int2(param.src_size) - 1;
    coord = clamp(coord, int2(0, 0), max_coord);
    return src_texture.Load(int3(coord, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    uint2 dst_coord = dispatch_thread_id.xy;
    if (dst_coord.x >= param.dst_size.x || dst_coord.y >= param.dst_size.y) {
        return;
    }

    if (param.is_mip0 != 0) {
        dst_texture[dst_coord] = load_src_depth(int2(dst_coord));
        return;
    }

    int2 src_coord = int2(dst_coord) << 1;
    float4 depth = float4(
        load_src_depth(src_coord),
        load_src_depth(src_coord + int2(1, 0)),
        load_src_depth(src_coord + int2(0, 1)),
        load_src_depth(src_coord + int2(1, 1))
    );

    dst_texture[dst_coord] = reduce_depth(depth);
}
