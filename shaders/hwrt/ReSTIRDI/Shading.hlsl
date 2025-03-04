#include <framework/DI/Bindings.hlsli>

#include <framework/DI/GridCommon.hlsli>
#include <framework/DI/Reservoirs.hlsli>
#include <framework/DI/Utils.hlsli>
#define WITH_NRD 1
#ifdef WITH_NRD
#include <nrd/NRD.hlsli>
#endif
#include <framework/DI/ShadingUtils.hlsli>

[numthreads(DI_SCREEN_TILE_SIZE, DI_SCREEN_TILE_SIZE, 1)] void
main(uint2 dtid
     : SV_DISPATCHTHREADID, uint2 gtid
     : SV_GROUPTHREADID, uint2 gid
     : SV_GROUPID) {
  Moer::DI::ReSTIRDIParams params = resample_params.restir_di_params;

  uint2 pixel_pos = dtid.xy;
  Moer::Surface surface = Moer::GetGBufferSurface(pixel_pos);

  Moer::DI::Reservoir res =
      Moer::DI::LoadReservoir(params.reservoir_buffer_params, pixel_pos,
                              params.buffer_indices.shading_input_buff_idx);
  float3 diffuse = 0.f;
  float3 specular = 0.f;
  float light_dist = 0.f;
  float2 cur_luminance = 0.f;

  float3 debug_color_red = float3(1, 0, 0);
  float3 debug_color_green = float3(0, 1, 0);
  bool b_use_red = false;
  float3 test_color = 0.f;
  float3 diffuse_prob = 0.f;
  if (res.IsValid()) {

    Moer::PolymorphicLightInfo light_info =
        Moer::LoadLightInfo(res.GetLightIndex());

    Moer::LightSample l_sample =
        surface.SamplePolymorphicLight(light_info, res.GetUV());
    bool b_store = Moer::ShadeSurface(
        res, surface, l_sample, resample_params.enable_prev_tlas,
        true /* visibility reuse */, diffuse, specular, light_dist);

    cur_luminance.x = STL::Color::Luminance(diffuse * surface.diffuse_albedo);
    cur_luminance.y = STL::Color::Luminance(specular);
    diffuse_prob.r = surface.diffuse_prob;
    specular /= max(surface.specular_f0, 0.001f);


    if (b_store) {
      Moer::DI::StoreReservoir(res, params.reservoir_buffer_params, pixel_pos,
                               params.buffer_indices.shading_input_buff_idx);
    }
    if (res.age > 0) {
      b_use_red = true;
    }
  }
  rw_restir_luminance[pixel_pos] = cur_luminance;

  // rw_diffuse_lighting[pixel_pos] = float4(diffuse, light_dist);
  // rw_specular_lighting[pixel_pos] = float4(specular, light_dist);

  Moer::StoreShadingResults(pixel_pos, surface.GetLinearDepth(),
                            surface.roughness, diffuse, specular, light_dist,
                            true);
}