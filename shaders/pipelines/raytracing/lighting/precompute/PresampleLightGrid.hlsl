#include <lighting/lib/restir/Bindings.hlsli>
#include <lighting/lib/restir/PresampleFunctions.hlsli>

[numthreads(DI_PRESAMPLE_GRID_SIZE, 1, 1)] void main(uint dtid
                                  : SV_DispatchThreadID) {
  Moer::RandomState rng =
      Moer::RandomState::Create(uint2(dtid & 0xfff, dtid >> 12), 1 * 13 + resample_params.frame_idx);
  Moer::RandomState coherent_rng =
      Moer::RandomState::Create(uint2(dtid >> 8, 0), 1 * 13 + resample_params.frame_idx);

  Moer::SampleFunc::SampleLocalLightsForGrid(
      rng, coherent_rng, dtid,
      resample_params.light_buffer_params.local_light_region,
      resample_params.local_light_ris_buffer_params,
      resample_params.grid_params);
}