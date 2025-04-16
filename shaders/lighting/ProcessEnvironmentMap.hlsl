#include <MathLib/STL.hlsli>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
static const float PI = 3.1415926535f;

[[vk::binding(0, 1)]] RWTexture2D<float> integrated_mips[] : register(u0);

[[vk::push_constant]] ConstantBuffer<Moer::PreprocessEnvironmentMapParams>
    param;

[[vk::binding(0, 0)]] Texture2D<float4> env_map : register(t0);

float GetPixelWeight(uint2 pos) {
  float3 color = env_map[pos].rgb;
  float lum = max(STL::Color::Luminance(color), 0.f);

  if (isinf(lum) || isnan(lum)) {
    return 0.f;
  }

  float elevation =
      ((float(pos.y) + 0.5f) / float(param.src_size.y) - 0.5f) * PI;
  float relative_solid_angle = cos(elevation);

  const float max_weight = 65504.f; // encode as 16 bit float

  return clamp(lum * relative_solid_angle, 0.f, max_weight);
}

groupshared float g_weights[16];

[numthreads(DI_PRESAMPLE_GRID_SIZE, 1, 1)] void main(uint2 gid
                                                     : SV_GroupID, uint tid
                                                     : SV_GroupThreadID) {
  uint2 local_idx = Math::LinearIndexToZCurve(tid);
  uint2 gtid = (gid << 4) + local_idx;

  float4 src_weights;

  if (param.src_mip_level == 0) {
    uint2 src_pos = gtid.xy << 1;
    src_weights =
        float4(GetPixelWeight(src_pos), GetPixelWeight(src_pos + uint2(1, 0)),
               GetPixelWeight(src_pos + uint2(0, 1)),
               GetPixelWeight(src_pos + uint2(1, 1)));

    RWTexture2D<float> dst = integrated_mips[param.src_mip_level];

    dst[src_pos + int2(0, 0)] = src_weights.x;
    dst[src_pos + int2(1, 0)] = src_weights.y;
    dst[src_pos + int2(0, 1)] = src_weights.z;
    dst[src_pos + int2(1, 1)] = src_weights.w;
  } else {
    uint2 src_pos = gtid.xy << 1;
    RWTexture2D<float> src = integrated_mips[param.src_mip_level];
    src_weights = float4(src[src_pos + int2(0, 0)], src[src_pos + int2(1, 0)],
                         src[src_pos + int2(0, 1)], src[src_pos + int2(1, 1)]);
  }

  uint mip_levels_to_write = param.num_mip_levels - param.src_mip_level - 1;
  if (mip_levels_to_write < 1)
    return;

  float weight =
      (src_weights.x + src_weights.y + src_weights.z + src_weights.w) * 0.25f;
  integrated_mips[param.src_mip_level + 1][gtid] = weight;

  if (mip_levels_to_write < 2)
    return;

  uint lane = WaveGetLaneIndex();
  weight =
      (weight + WaveReadLaneAt(weight, lane + 1) +
       WaveReadLaneAt(weight, lane + 2) + WaveReadLaneAt(weight, lane + 3)) *
      0.25f;

  if ((lane & 3) == 0) {
    integrated_mips[param.src_mip_level + 2][gtid >> 1] = weight;
  }

  if (mip_levels_to_write < 3)
    return;

  weight =
      (weight + WaveReadLaneAt(weight, lane + 4) +
       WaveReadLaneAt(weight, lane + 8) + WaveReadLaneAt(weight, lane + 12)) *
      0.25f;

  if ((lane & 15) == 0) {
    integrated_mips[param.src_mip_level + 3][gtid >> 2] = weight;

    g_weights[tid >> 4] = weight;
  }

  if (mip_levels_to_write < 4)
    return;

  GroupMemoryBarrierWithGroupSync();

  if (tid >= 16)
    return;

  weight = g_weights[tid];

  gtid = (gid << 1) + (local_idx >> 1);

  weight =
      (weight + WaveReadLaneAt(weight, lane + 1) +
       WaveReadLaneAt(weight, lane + 2) + WaveReadLaneAt(weight, lane + 3)) *
      0.25f;

  if ((lane & 3) == 0) {
    integrated_mips[param.src_mip_level + 4][gtid] = weight;
  }

  if (mip_levels_to_write < 5)
    return;

  weight =
      (weight + WaveReadLaneAt(weight, lane + 4) +
       WaveReadLaneAt(weight, lane + 8) + WaveReadLaneAt(weight, lane + 12)) *
      0.25f;

  if (lane == 0) {
    integrated_mips[param.src_mip_level + 5][gtid >> 1] = weight;
  }
}