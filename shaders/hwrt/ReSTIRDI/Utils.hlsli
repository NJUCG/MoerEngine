#ifndef MOER_HWRT_RESTIRDI_UTILS_HLSLI
#define MOER_HWRT_RESTIRDI_UTILS_HLSLI

namespace Moer {

#ifdef USE_BOILING_FILTER
#define BOILING_FILTER_MIN_LANE 32
#ifndef BOILING_FILTER_GROUP_SIZE
#error "BOILING_FILTER_GROUP_SIZE must be defined"
#endif
groupshared float
    s_weight[(BOILING_FILTER_GROUP_SIZE * BOILING_FILTER_GROUP_SIZE +
              BOILING_FILTER_GROUP_SIZE - 1) /
             BOILING_FILTER_GROUP_SIZE];

groupshared uint
    s_count[(BOILING_FILTER_GROUP_SIZE * BOILING_FILTER_GROUP_SIZE +
             BOILING_FILTER_GROUP_SIZE - 1) /
            BOILING_FILTER_GROUP_SIZE];

bool BoilingFilter(uint2 _local_idx, float _strength, float _reservoir_weight) {

  float boliing_filter_multiplier = 10.f / clamp(_strength, 1e-6, 1.f) - 9.f;

  float wave_weight = WaveActiveSum(_reservoir_weight);
  uint wave_cnt = WaveActiveCountBits(_reservoir_weight > 0);

  uint linear_thread_idx =
      _local_idx.y * BOILING_FILTER_GROUP_SIZE + _local_idx.x;
  uint wave_idx = linear_thread_idx / WaveGetLaneCount();

  // First lane of each wave writes the sum result to shared memory
  if (WaveIsFirstLane()) {
    s_weight[wave_idx] = wave_weight;
    s_count[wave_idx] = wave_cnt;
  }

  GroupMemoryBarrierWithGroupSync();

  // First wave reads the sum result from shared memory and writes the final
  // result
  if (linear_thread_idx <
      (BOILING_FILTER_GROUP_SIZE * BOILING_FILTER_GROUP_SIZE +
       WaveGetLaneCount() - 1) /
          WaveGetLaneCount()) {
    wave_weight = s_weight[linear_thread_idx];
    wave_cnt = s_count[linear_thread_idx];

    wave_weight = WaveActiveSum(wave_weight);
    wave_cnt = WaveActiveSum(wave_cnt);

    if (linear_thread_idx == 0) {
      s_weight[0] = (wave_cnt > 0) ? wave_weight / wave_cnt : 0;
    }
  }

  GroupMemoryBarrierWithGroupSync();

  float avg_weight = s_weight[0];
  if (_reservoir_weight > avg_weight * boliing_filter_multiplier) {
    return true;
  }

  return false;
}
#endif
} // namespace Moer
#endif // MOER_HWRT_RESTIRDI_UTILS_HLSLI