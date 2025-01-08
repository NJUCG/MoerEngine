#include <framework/Common.hlsl>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/ShaderParameters.h>

[[vk::binding(0, 0)]] RWTexture2D<float4> mips[] : register(u0);

[[vk::push_constant]] ConstantBuffer<Moer::BuildMipsParam> param;

groupshared float4 g_colors[16];

[numthreads(256, 1, 1)] void main(uint2 gid
                                  : SV_GroupID, uint tid
                                  : SV_GroupThreadID) {
  uint2 local_idx = Math::LinearIndexToZCurve(tid);
  uint2 gtid = (gid * 16) + local_idx;
  float4 src_colors[4];
  {
    uint2 src_pos = gtid.xy << 1;
    RWTexture2D<float4> src = mips[param.src_mip_level];
    src_colors[0] = src[src_pos + int2(0, 0)];
    src_colors[1] = src[src_pos + int2(1, 0)];
    src_colors[2] = src[src_pos + int2(0, 1)];
    src_colors[3] = src[src_pos + int2(1, 1)];

    //print src colors
    // if(param.src_mip_level == 5)
    // printf("src_colors[0] %f %f %f %f\n", src_colors[0].x, src_colors[0].y, src_colors[0].z, src_colors[0].w);

  }

  uint mip_levels_to_write = param.num_mip_levels - param.src_mip_level - 1;

  if (mip_levels_to_write < 1)
    return;

  float4 color =
      (src_colors[0] + src_colors[1] + src_colors[2] + src_colors[3]) * 0.25f;
  mips[param.src_mip_level + 1][gtid] = color;

  if (mip_levels_to_write < 2)
    return;

  uint lane = WaveGetLaneIndex();
  color = (color + WaveReadLaneAt(color, lane + 1) +
           WaveReadLaneAt(color, lane + 2) + WaveReadLaneAt(color, lane + 3)) *
          0.25f;

  if ((lane & 3) == 0) {
    mips[param.src_mip_level + 2][gtid >> 1] = color;
  }

  if (mip_levels_to_write < 3)
    return;

  color = (color + WaveReadLaneAt(color, lane + 4) +
           WaveReadLaneAt(color, lane + 8) + WaveReadLaneAt(color, lane + 12)) *
          0.25f;

  if ((lane & 15) == 0) {
    mips[param.src_mip_level + 3][gtid >> 2] = color;

    g_colors[tid >> 4] = color;
  }

  if (mip_levels_to_write < 4)
    return;

  GroupMemoryBarrierWithGroupSync();

  if (tid >= 16)
    return;

  color = g_colors[tid];

  gtid = (gid << 1) + (local_idx >> 1);

  color = (color + WaveReadLaneAt(color, lane + 1) +
           WaveReadLaneAt(color, lane + 2) + WaveReadLaneAt(color, lane + 3)) *
          0.25f;

  if ((lane & 3) == 0) {
    mips[param.src_mip_level + 4][gtid] = color;
  }

  if (mip_levels_to_write < 5)
    return;

  color = (color + WaveReadLaneAt(color, lane + 4) +
           WaveReadLaneAt(color, lane + 8) + WaveReadLaneAt(color, lane + 12)) *
          0.25f;

  if (lane == 0) {
    mips[param.src_mip_level + 5][gtid >> 1] = color;
  }
}