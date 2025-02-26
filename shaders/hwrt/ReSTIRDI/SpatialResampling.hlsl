#include <framework/DI/Bindings.hlsli>
#include <framework/DI/ReSampleFunctions.hlsli>
#include <hwrt/GBufferUtils.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid
     : SV_DispatchThreadID) {
  uint2 pixel_pos = dtid.xy;
  // printf("pixel_pos: %d %d\n", pixel_pos.x, pixel_pos.y);

  Moer::RandomState rng = Moer::RandomState::Create(pixel_pos, 3 * 13 + resample_params.frame_idx);

  Moer::Surface surface = Moer::GetGBufferSurface(pixel_pos);

  Moer::LightSample light_sample;

  Moer::DI::Reservoir res = Moer::DI::Reservoir::EmptyReservoir();
  int2 temporal_pixel_pos = -1;

  if (surface.IsValid()) {
    Moer::DI::Reservoir cur_res = Moer::DI::LoadReservoir(
        resample_params.restir_di_params.reservoir_buffer_params, pixel_pos,
        resample_params.restir_di_params.buffer_indices
            .initial_sample_output_buff_idx);

    Moer::DI::SpatialResampleParams s_params;

    s_params.src_buffer_idx = resample_params.restir_di_params.buffer_indices
                                  .spatial_resample_input_buff_idx;
    s_params.num_samples = resample_params.restir_di_params.spatial_resample_params.num_spatial_samples;                                  
    s_params.max_history_length =
        resample_params.restir_di_params.temporal_resample_params
            .max_history_length;
    s_params.bias_correction_mode =
        resample_params.restir_di_params.spatial_resample_params
            .bias_correction_mode;
    s_params.depth_threshold = resample_params.restir_di_params
                                   .spatial_resample_params.depth_threshold;
    s_params.normal_threshold = resample_params.restir_di_params
                                    .spatial_resample_params.normal_threshold;
    s_params.sampling_radius =
        resample_params.restir_di_params.spatial_resample_params
            .radius;
    s_params.num_disocclusion_samples = resample_params.restir_di_params
                                            .spatial_resample_params.num_disocclusion_samples;
    s_params.b_test_material_similarity = true;                               
    s_params.discount_native_samples = resample_params.restir_di_params
                                            .spatial_resample_params.discount_native_samples;

    Moer::LightSample selected_sample = (Moer::LightSample)0;
    res = Moer::DI::SpatialResampling(
        pixel_pos, surface, cur_res, rng, resample_params.di_params,
        resample_params.restir_di_params.reservoir_buffer_params, s_params,
        selected_sample);
  }

  Moer::DI::StoreReservoir(
      res, resample_params.restir_di_params.reservoir_buffer_params, pixel_pos,
      resample_params.restir_di_params.buffer_indices
          .spatial_resample_output_buff_idx);
}