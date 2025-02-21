#include <framework/DI/Bindings.hlsli>

#include <framework/DI/GridCommon.hlsli>
#include <framework/DI/Reservoirs.hlsli>
#include <framework/DI/Utils.hlsli>

#ifdef WITH_NRD
#include <nrd/NRD.hlsli>
#endif

bool ShadeSurface(inout Moer::DI::Reservoir _res, Moer::Surface _surface,
                  Moer::LightSample _sample, bool _b_prev_tlas,
                  bool _b_visibility_reuse, out float3 _diffuse,
                  out float3 _specular, out float _light_dist) {
  _diffuse = 0.f;
  _specular = 0.f;
  _light_dist = 0.f;

  if (_sample.solid_angle_pdf <= 0.f)
    return false;

  bool b_store = false;
  if (resample_params.restir_di_params.shading_params.enable_final_visiblity) {
    float3 vis = 0.f;
    bool reused = false;
    Moer::DI::VisibilityResuseParam reuse_param;
    reuse_param.max_age =
        resample_params.restir_di_params.shading_params.final_visiblity_max_age;
    reuse_param.max_distance = resample_params.restir_di_params.shading_params
                                   .final_visiblity_max_distance;
    if (resample_params.restir_di_params.shading_params.reuse_final_visiblity) {
      reused = _res.GetVisibility(reuse_param, vis);
    }

    if (!reused) {
      if (_b_prev_tlas && resample_params.enable_prev_tlas) {
        vis = Moer::GetFinalVisibility(prev_tlas, _surface, _sample.x);
      } else {
        vis = Moer::GetFinalVisibility(tlas, _surface, _sample.x);
      }
      _res.StoreVisibility(
          vis, resample_params.restir_di_params.temporal_resample_params
                   .discard_inviable_samples);
      b_store = true;
    }

    _sample.radiance *= vis;
  }
  _sample.radiance *= _res.GetInvPdf() / _sample.solid_angle_pdf;
  
  if (any(_sample.radiance > 0.f)) {
    float diffuse_term = 0.f;
    float3 specular_term = 0.f;
    _surface.EvalBrdf(_sample.x, diffuse_term, specular_term);

    _diffuse = _sample.radiance * diffuse_term;
    _specular = _sample.radiance * specular_term;

    _light_dist = length(_sample.x - _surface.GetWorldPos());
  }

  return b_store;
}

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
  if (res.IsValid()) {

    Moer::PolymorphicLightInfo light_info =
        Moer::LoadLightInfo(res.GetLightIndex());

    Moer::LightSample l_sample =
        surface.SamplePolymorphicLight(light_info, res.GetUV());
    bool b_store = ShadeSurface(
        res, surface, l_sample, resample_params.enable_prev_tlas,
        true /* visibility reuse */, diffuse, specular, light_dist);

    cur_luminance.x = STL::Color::Luminance(diffuse * surface.diffuse_albedo);
    cur_luminance.y = STL::Color::Luminance(specular);

    specular = specular / max(0.001f, surface.specular_f0);
    diffuse *= surface.diffuse_albedo;

    // printf("diffuse: %f %f %f\n", diffuse.x, diffuse.y, diffuse.z);

    if (b_store) {
      Moer::DI::StoreReservoir(res, params.reservoir_buffer_params, pixel_pos,
                               params.buffer_indices.shading_input_buff_idx);
    }
    if(res.age > 0){
      b_use_red = true;
    }
  }

  rw_restir_luminance[pixel_pos] = cur_luminance;

  rw_diffuse_lighting[pixel_pos] = float4(diffuse, light_dist);
  rw_specular_lighting[pixel_pos] = float4(specular, light_dist);
  // rw_specular_lighting[pixel_pos] = float4(b_use_red ? debug_color_red : debug_color_green, light_dist);

  //test
  // rw_diffuse_lighting[pixel_pos] = float4(diffuse + specular, light_dist);
}