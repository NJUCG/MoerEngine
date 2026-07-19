#include <pipelines/raytracing/lighting/lib/restir/Bindings.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/InitialSampleFunctions.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid
     : SV_DispatchThreadID) {
  uint2 pixel_pos = dtid.xy;

  Moer::RandomState rng = Moer::RandomState::Create(pixel_pos, 1 * 13 + resample_params.frame_idx);
  Moer::RandomState tile_rng =
      Moer::RandomState::Create(pixel_pos / float(DI_SCREEN_TILE_SIZE), 1 * 13 + resample_params.frame_idx);

  Moer::Surface surface = Moer::GetGBufferSurface(pixel_pos);

  Moer::DI::SampleConfigs params = Moer::DI::SampleConfigs::Create(
      resample_params.restir_di_params.initial_sample_params
          .num_primary_local_lights,
      resample_params.restir_di_params.initial_sample_params
          .num_primary_infinite_lights,
      resample_params.restir_di_params.initial_sample_params
          .num_primary_env_lights,
      resample_params.restir_di_params.initial_sample_params
          .num_primary_brdf_lights,
      resample_params.restir_di_params.initial_sample_params.brdf_cutoff);

  Moer::LightSample light_sample;

  Moer::DI::Reservoir res = Moer::DI::SampleLightsForSurface(
      rng, tile_rng, params, surface, resample_params.light_buffer_params,
      resample_params.local_light_ris_buffer_params,
      resample_params.env_light_ris_buffer_params, resample_params.grid_params,
      resample_params.restir_di_params.initial_sample_params
          .local_light_sample_mode,
      light_sample);

  if (resample_params.restir_di_params.initial_sample_params
          .enable_initial_visibility &&
      res.IsValid()) {
    if (!Moer::GetCurrentConservativeVisibility(surface, light_sample.x)) {
      res.StoreVisibility(0.f, true);
    }
  }

  Moer::DI::StoreReservoir(
      res, resample_params.restir_di_params.reservoir_buffer_params, pixel_pos,
      resample_params.restir_di_params.buffer_indices
          .initial_sample_output_buff_idx);

}
