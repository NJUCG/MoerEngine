#ifndef MOER_FRAMEWORK_DI_SHADINGUTILS_HLSLI
#define MOER_FRAMEWORK_DI_SHADINGUTILS_HLSLI
namespace Moer{

#ifdef MOER_FRAMEWORK_DI_RESERVOIRS_HLSLI
bool ShadeSurface(inout DI::Reservoir _res, Surface _surface,
                  LightSample _sample, bool _b_prev_tlas,
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
    DI::VisibilityResuseParam reuse_param;
    reuse_param.max_age =
        resample_params.restir_di_params.shading_params.final_visiblity_max_age;
    reuse_param.max_distance = resample_params.restir_di_params.shading_params
                                   .final_visiblity_max_distance;
    if (resample_params.restir_di_params.shading_params.reuse_final_visiblity) {
      reused = _res.GetVisibility(reuse_param, vis);
    }

    if (!reused) {
      if (_b_prev_tlas && resample_params.enable_prev_tlas) {
        vis = GetFinalVisibility(prev_tlas, _surface, _sample.x);
      } else {
        vis = GetFinalVisibility(tlas, _surface, _sample.x);
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
    float3 diffuse_term = 0.f;
    float3 specular_term = 0.f;
    _surface.EvalBrdf(_sample.x, diffuse_term, specular_term);

    _diffuse = _sample.radiance * diffuse_term;
    _specular = _sample.radiance * specular_term;

    _light_dist = length(_sample.x - _surface.GetWorldPos());
  }

  return b_store;
}

#endif // MOER_FRAMEWORK_DI_RESERVOIRS_HLSLI

void StoreShadingResults(uint2 _pixel_pos, float _view_depth, float _roughness,
                         float3 _diffuse, float3 _specular, float _light_dist,
                         bool _is_first_pass) {
#ifdef WITH_NRD
  if (resample_params.denoiser_mode != Moer::s_denoiser_mode_off) {

    bool b_relax = resample_params.denoiser_mode == Moer::s_denoiser_mode_relax;
    const bool santitize = true;

    if (b_relax) {
      rw_diffuse_lighting[_pixel_pos] = RELAX_FrontEnd_PackRadianceAndHitDist(
          _diffuse, _light_dist, santitize);
      rw_specular_lighting[_pixel_pos] = RELAX_FrontEnd_PackRadianceAndHitDist(
          _specular, _light_dist, santitize);
    } else {
      float diff_norm_dist = REBLUR_FrontEnd_GetNormHitDist(
          _light_dist, _view_depth, resample_params.reblur_diff_hit_dist_params,
          1.f);
      rw_diffuse_lighting[_pixel_pos] =
          REBLUR_FrontEnd_PackRadianceAndNormHitDist(_diffuse, diff_norm_dist,
                                                     santitize);

      float spec_norm_dist = REBLUR_FrontEnd_GetNormHitDist(
          _light_dist, _view_depth, resample_params.reblur_spec_hit_dist_params,
          1.f);
      rw_specular_lighting[_pixel_pos] =
          REBLUR_FrontEnd_PackRadianceAndNormHitDist(_specular, spec_norm_dist,
                                                     santitize);
    }
  } else

#endif
  {
    rw_diffuse_lighting[_pixel_pos] = float4(_diffuse, _light_dist);
    rw_specular_lighting[_pixel_pos] = float4(_specular, _light_dist);
  }
}

}
#endif  // MOER_FRAMEWORK_DI_SHADINGUTILS_HLSLI