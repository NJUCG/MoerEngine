#define BOILING_FILTER_GROUP_SIZE DI_SCREEN_TILE_SIZE
#define USE_BOILING_FILTER 1

#include <pipelines/raytracing/lighting/lib/restir/Bindings.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/ResampleFunctions.hlsli>
#include <pipelines/raytracing/passes/GBufferUtils.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid
     : SV_DispatchThreadID, uint2 gtid
     : SV_GroupThreadID) {
  uint2 pixel_pos = dtid.xy;
  // printf("pixel_pos: %d %d\n", pixel_pos.x, pixel_pos.y);

  Moer::RandomState rng =
      Moer::RandomState::Create(pixel_pos, 2 * 13 + resample_params.frame_idx);

  Moer::Surface surface = Moer::GetGBufferSurface(pixel_pos);

  bool use_permutation_sampling =
      resample_params.restir_di_params.temporal_resample_params
          .enable_permutation_sample;
  use_permutation_sampling = false;
  Moer::LightSample light_sample;

  Moer::DI::Reservoir res = Moer::DI::Reservoir::EmptyReservoir();
  int2 temporal_pixel_pos = -1;

  if (surface.IsValid()) {
    Moer::DI::Reservoir cur_res = Moer::DI::LoadReservoir(
        resample_params.restir_di_params.reservoir_buffer_params, pixel_pos,
        resample_params.restir_di_params.buffer_indices
            .initial_sample_output_buff_idx);

    TextureHandle motion_handle =
        TextureHandle(resample_params.bindless_handles.motion);
    Texture2D<float3> motion_tex = motion_handle.GetTexture2D<float3>();
    float3 motion = motion_tex[pixel_pos].xyz;
    motion =
        Moer::MotionToPixelSpace(resample_params.main_view,
                                 resample_params.prev_view, pixel_pos, motion);
    Moer::DI::TemporalResampleParams t_params;
    t_params.screen_motion = motion;
    t_params.src_buffer_idx = resample_params.restir_di_params.buffer_indices
                                  .temperal_resample_input_buff_idx;
    t_params.max_history_length =
        resample_params.restir_di_params.temporal_resample_params
            .max_history_length;
    t_params.bias_correction_mode =
        resample_params.restir_di_params.temporal_resample_params
            .bias_correction_mode;
    t_params.depth_threshold = resample_params.restir_di_params
                                   .temporal_resample_params.depth_threshold;
    t_params.normal_threshold = resample_params.restir_di_params
                                    .temporal_resample_params.normal_threshold;
    t_params.enable_prior_visibility =
        resample_params.restir_di_params.temporal_resample_params
            .discard_inviable_samples;
    t_params.enable_permutation_sampling = use_permutation_sampling;
    t_params.random_seed =
        resample_params.restir_di_params.temporal_resample_params.random_number;

    Moer::LightSample selected_sample = (Moer::LightSample)0;

    res = Moer::DI::TemporalResampling(
        pixel_pos, surface, cur_res, rng, resample_params.di_params,
        resample_params.restir_di_params.reservoir_buffer_params, t_params,
        temporal_pixel_pos, selected_sample);
  }
#ifdef USE_BOILING_FILTER
  if (resample_params.restir_di_params.temporal_resample_params
          .enbale_boiling_filter) {
    Moer::DI::BoilingFilter(gtid,
                            resample_params.restir_di_params
                                .temporal_resample_params.boiling_filter_scale,
                            res);
  }
#endif

  Moer::DI::StoreReservoir(
      res, resample_params.restir_di_params.reservoir_buffer_params, pixel_pos,
      resample_params.restir_di_params.buffer_indices
          .temperal_resample_output_buff_idx);
}
