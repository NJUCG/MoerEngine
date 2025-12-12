#include <core/math/Math.hlsli>
#include <shared/ShaderParameters.h>
#include <shared/utils/Packing.h>

#ifndef WITH_NRD
#define WITH_NRD 1
#endif

#if WITH_NRD
#include <external/nrd/NRD.hlsli>
#endif

[[vk::binding(1, 0)]] RWTexture2D<uint> rw_specular_roughness : register(u0);
[[vk::binding(2, 0)]] RWTexture2D<float4> rw_normal_roughness : register(u1);
[[vk::binding(3, 0)]] Texture2D<uint> r_normal : register(t0);
[[vk::binding(4, 0)]] Texture2D<float> r_view_depth : register(t1);

// FROM NRD
#define NRD_BILATERAL_WEIGHT_VIEWZ_SENSITIVITY 100.0
#define NRD_BILATERAL_WEIGHT_CUTOFF 0.03

static const float s_mirror_roughness = 0.01f;

float GetBilateralWeight(float _z, float _zc) {

  _z = abs(_z - _zc) * rcp(min(abs(_z), abs(_zc)) + 0.001);
  _z = rcp(1.f + NRD_BILATERAL_WEIGHT_VIEWZ_SENSITIVITY * _z) *
       step(_z, NRD_BILATERAL_WEIGHT_CUTOFF);
  return _z;
}

float GetModifiedRoughnessFromNormalVariance(float _roughness,
                                             float3 _avg_normal) {

  // https://blog.selfshadow.com/publications/s2013-shading-course/rad/s2013_pbs_rad_notes.pdf
  // (page 20)
  float l = length(_avg_normal);
  float kappa = saturate(1.f - l * l) * rcp(max(l * (3.f - l * l), 1e-15));
  return sqrt(saturate(_roughness * _roughness + kappa));
}

[numthreads(16, 16, 1)] void main(uint2 _pixel_pos
                                  : SV_DISPATCHTHREADID) {
  float3 normal = Math::OctToNdirUnorm32(r_normal[_pixel_pos]);
  float4 spec_roughness =
      Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(rw_specular_roughness[_pixel_pos]);

  float view_z = r_view_depth[_pixel_pos];
  float roughness = spec_roughness.w;

  float3 avg_normal = normal;
  float sum_w = 1.f;

  // compute sum

  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) {
      if (i == 0 && j == 0)
        continue;
      uint2 pos = _pixel_pos + uint2(i, j);

      float3 p_normal = Math::OctToNdirUnorm32(r_normal[pos]);
      float p_z = r_view_depth[pos];

      float w = GetBilateralWeight(view_z, p_z);

      avg_normal += w * p_normal;
      sum_w += w;
    }
  }

  float inv_sum_w = 1.f / sum_w;
  avg_normal *= inv_sum_w;

  float roughness_mod;
  if (roughness <= s_mirror_roughness) {
    roughness_mod = 0.f;
  } else {
    roughness_mod =
        GetModifiedRoughnessFromNormalVariance(roughness, avg_normal);
  }
  rw_specular_roughness[_pixel_pos] = Moer::Pack_R8G8B8A8_Gamma_UFLOAT(
      float4(spec_roughness.xyz, roughness_mod));

#if WITH_NRD
  rw_normal_roughness[_pixel_pos] =
      NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0u);
#endif
}
