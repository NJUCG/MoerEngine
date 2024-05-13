#ifndef HIZ_BATCH_CNT
#define HIZ_BATCH_CNT 1
#endif
struct HiZConfig {
  bool b_mip0;
  uint target_level;
  uint2 size;
};

groupshared float group_depth[16][16];

[[vk::binding(0, 0)]] RWTexture2D<float> target0 : register(u0);
[[vk::binding(1, 0)]] RWTexture2D<float> target1 : register(u1);
[[vk::binding(2, 0)]] RWTexture2D<float> target2 : register(u2);
[[vk::binding(3, 0)]] RWTexture2D<float> target3 : register(u3);

[[vk::push_constant]] ConstantBuffer<HiZConfig> config : register(b0);

[[vk::binding(0, 1)]] SamplerState depth_sampler : register(t0);
[[vk::binding(0, 2)]] Texture2D<float> depth_buffer : register(t1);

#define SetMip(level, coord, val) target##level[coord] = val;

void GenerateMip0(uint3 dtid) {
  uint2 size = config.size;
  uint2 gid = dtid.xy;
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  float2 uv = (gid + 0.5f) / float2(size.x, size.y);
  float depth = depth_buffer.SampleLevel(depth_sampler, uv, 0);
  // reverse y coord
  gid.y = size.y - gid.y - 1;
  SetMip(0, gid, depth);
}

// void GenerateMipN(uint level, uint3 dtid) {
//   uint last_level = level - 1;

//   uint2 size = config.size; // example: target mip1 use mip0 size dispatch
//   uint2 gid = dtid.xy;
//   if (gid.x >= size.x || gid.y >= size.y) {
//     return;
//   }
//   uint2 coord = gid << 1;
//   float4 depth =
//       float4(depth_buffer.Load(int3(coord, last_level)),
//              depth_buffer.Load(int3(coord + int2(1, 0), last_level)),
//              depth_buffer.Load(int3(coord + int2(0, 1), last_level)),
//              depth_buffer.Load(int3(coord + int2(1, 1), last_level)));

//   depth.xy = min(depth.xy, depth.zw);
//   depth.x = min(depth.x, depth.y);
//   target[gid] = depth.x;
// }

void GenerateMipNBatch(uint start_level, uint3 dtid, uint2 thread_id) {
  uint last_level = start_level - 1;

  uint2 size = config.size; // example: target mip1 use mip0 size dispatch
  uint2 gid = dtid.xy;
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }

  float sampled_depth = depth_buffer.Load(int3(gid, last_level));
  group_depth[thread_id.x][thread_id.y] = sampled_depth;

  GroupMemoryBarrierWithGroupSync();

  // calc 4 min neighbors
  if (thread_id.x % 2 == 0 && thread_id.y % 2 == 0) {
    float4 depth = float4(group_depth[thread_id.x][thread_id.y],
                          group_depth[thread_id.x + 1][thread_id.y],
                          group_depth[thread_id.x][thread_id.y + 1],
                          group_depth[thread_id.x + 1][thread_id.y + 1]);
    depth.xy = min(depth.xy, depth.zw);
    depth.x = min(depth.x, depth.y);
    group_depth[thread_id.x][thread_id.y] = depth.x;
    SetMip(0, gid >> 1, depth.x);
  }

#if HIZ_BATCH_CNT > 1
  GroupMemoryBarrierWithGroupSync();
  if (thread_id.x % (1 << 2) == 0 && thread_id.y % (1 << 2) == 0) {
    float4 depth =
        float4(group_depth[thread_id.x][thread_id.y],
               group_depth[thread_id.x + (1 << 1)][thread_id.y],
               group_depth[thread_id.x][thread_id.y + (1 << 1)],
               group_depth[thread_id.x + (1 << 1)][thread_id.y + (1 << 1)]);
    depth.xy = min(depth.xy, depth.zw);
    depth.x = min(depth.x, depth.y);
    group_depth[thread_id.x][thread_id.y] = depth.x;
    SetMip(1, gid >> 2, depth.x);
  }

#endif

#if HIZ_BATCH_CNT > 2
  GroupMemoryBarrierWithGroupSync();
  if (thread_id.x % (1 << 3) == 0 && thread_id.y % (1 << 3) == 0) {
    float4 depth =
        float4(group_depth[thread_id.x][thread_id.y],
               group_depth[thread_id.x + (1 << 2)][thread_id.y],
               group_depth[thread_id.x][thread_id.y + (1 << 2)],
               group_depth[thread_id.x + (1 << 2)][thread_id.y + (1 << 2)]);
    depth.xy = min(depth.xy, depth.zw);
    depth.x = min(depth.x, depth.y);
    group_depth[thread_id.x][thread_id.y] = depth.x;
    SetMip(2, gid >> 3, depth.x);
  }

#endif

#if HIZ_BATCH_CNT > 3
  GroupMemoryBarrierWithGroupSync();
  if (thread_id.x % (1 << 4) == 0 && thread_id.y % (1 << 4) == 0) {
    float4 depth =
        float4(group_depth[thread_id.x][thread_id.y],
               group_depth[thread_id.x + (1 << 3)][thread_id.y],
               group_depth[thread_id.x][thread_id.y + (1 << 3)],
               group_depth[thread_id.x + (1 << 3)][thread_id.y + (1 << 3)]);
    depth.xy = min(depth.xy, depth.zw);
    depth.x = min(depth.x, depth.y);
    group_depth[thread_id.x][thread_id.y] = depth.x;
    SetMip(3, gid >> 4, depth.x);
  }

#endif
}

[numthreads(16, 16, 1)] void main(uint3 dtid
                                  : SV_DispatchThreadID, uint2 thread_id
                                  : SV_GroupThreadID) {
  [branch] if (config.b_mip0) { GenerateMip0(dtid); }
  else {
    // GenerateMipN(config.target_level, dtid);
    GenerateMipNBatch(config.target_level, dtid, thread_id);
  }
}