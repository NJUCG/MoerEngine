#include <Mathlib/STL.hlsli>
#include <shared/postprocess/ShaderParameters.h>


[[vk::binding(0, 0)]] ConstantBuffer<Moer::ToneMappingParams> params
    : register(b0);
[[vk::binding(1, 0)]] Texture2D source_tex : register(t0);
[[vk::binding(2, 0)]] RWBuffer<uint> histogram : register(u0);

#define GROUP_X 16
#define GROUP_Y 16
#define POINT_FRAC_BITS 6
#define POINT_FRAC_MULTIPLIER (1 << POINT_FRAC_BITS)

groupshared uint s_histogram[HISTOGRAM_BINS];

[numthreads(GROUP_X, GROUP_Y, 1)] void main(uint _gid
                                            : SV_GROUPINDEX, uint2 _dtid
                                            : SV_DISPATCHTHREADID) {
  uint2 pixel_pos = _dtid.xy + params.view_origin.xy;
  bool valid = all(_dtid < params.view_size.xy);

  if (_gid < HISTOGRAM_BINS) {
    s_histogram[_gid] = 0;
  }

  GroupMemoryBarrierWithGroupSync();
  if (valid) {
    float3 color = source_tex[pixel_pos].rgb;
    float luminance = STL::Color::Luminance(color);
    float biased_log_lum = log2(luminance) * params.log_luminance_scale +
                           params.log_luminance_bias;
    float histogram_bin = saturate(biased_log_lum) * (HISTOGRAM_BINS - 1);

    uint left_bin = uint(floor(histogram_bin));
    uint right_bin = left_bin + 1;

    uint right_weight = uint(frac(histogram_bin) * POINT_FRAC_MULTIPLIER);
    uint left_weight = POINT_FRAC_MULTIPLIER - right_weight;

    if (left_weight != 0 && left_bin < HISTOGRAM_BINS) {
      InterlockedAdd(s_histogram[left_bin], left_weight);
    }

    if (right_weight != 0 && right_bin < HISTOGRAM_BINS) {
      InterlockedAdd(s_histogram[right_bin], right_weight);
    }
  }

  GroupMemoryBarrierWithGroupSync();

  if (_gid < HISTOGRAM_BINS) {
    uint local_bin_val = s_histogram[_gid];
    if (local_bin_val != 0) {
      InterlockedAdd(histogram[_gid], local_bin_val);
    }
  }
}