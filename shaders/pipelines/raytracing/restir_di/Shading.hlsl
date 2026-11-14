#include <pipelines/raytracing/lighting/lib/restir/Bindings.hlsli>

#include <pipelines/raytracing/lighting/lib/restir/GridCommon.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/Reservoirs.hlsli>
#include <pipelines/raytracing/lighting/lib/restir/Utils.hlsli>

#ifndef WITH_NRD
#define WITH_NRD 1
#endif

#if WITH_NRD
#include <external/nrd/NRD.hlsli>
#endif

// The following header must after WITH_NRD & NRD header
#include <pipelines/raytracing/lighting/lib/restir/ShadingUtils.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid
     : SV_DISPATCHTHREADID) {
  Moer::DI::ReSTIRDIParams params = resample_params.restir_di_params;

  uint2 pixel_pos = dtid.xy;
  Moer::Surface surface = Moer::GetGBufferSurface(pixel_pos);

  Moer::DI::Reservoir res =
      Moer::DI::LoadReservoir(params.reservoir_buffer_params, pixel_pos,
                              params.buffer_indices.shading_input_buff_idx);
  float3 diffuse = 0.f;
  float3 specular = 0.f;
  float light_dist = 0.f;

  if (res.IsValid()) {

    Moer::PolymorphicLightInfo light_info =
        Moer::LoadLightInfo(res.GetLightIndex());

    Moer::LightSample l_sample =
        surface.SamplePolymorphicLight(light_info, res.GetUV());
    bool b_store = Moer::ShadeSurface(
        res, surface, l_sample, resample_params.enable_prev_tlas,
        true /* visibility reuse */, diffuse, specular, light_dist);

    specular /= max(surface.specular_f0, 0.001f);


    if (b_store) {
      Moer::DI::StoreReservoir(res, params.reservoir_buffer_params, pixel_pos,
                               params.buffer_indices.shading_input_buff_idx);
    }
  }

  Moer::StoreShadingResults(pixel_pos, surface.GetLinearDepth(),
                            diffuse, specular, light_dist);
}
