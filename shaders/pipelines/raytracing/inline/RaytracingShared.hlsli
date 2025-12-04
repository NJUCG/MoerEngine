#ifndef MOER_RT_SHARED_HLSL
#define MOER_RT_SHARED_HLSL

#include "core/math/STL.hlsli"
#include "shared/ShaderParameters.h"


#pragma region[ rt geometry ]

struct RTVertex {
  float3 position;
  float uv0;
  float3 normal;
  float uv1;
  float3 tangent;
  float padding;
};

#ifndef VULKAN
// TODO: This code is not needed if HLSL 2021 is enabled. But currently
// it works only for latest DXC from VK SDK. DXC from Win SDK crashes!
// Keep an eye on "--hlsl2021" used in Cmake and C++.
int3 select(bool3 cmp, int3 a, int3 b) {
  int3 r;
  r.x = cmp.x ? a.x : b.x;
  r.y = cmp.y ? a.y : b.y;
  r.z = cmp.z ? a.z : b.z;

  return r;
}

float3 select(bool3 cmp, float3 a, float3 b) {
  float3 r;
  r.x = cmp.x ? a.x : b.x;
  r.y = cmp.y ? a.y : b.y;
  r.z = cmp.z ? a.z : b.z;

  return r;
}
#endif

float3 _GetXoffset(float3 X, float3 N) {
  // TODO: try out:
  // https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/

  // RT Gems "A Fast and Robust Method for Avoiding Self-Intersection" ( updated
  // version taken from Falcor ) Moves the ray origin further from surface to
  // prevent self-intersections, minimizes the distance.
  const float origin = 1.0 / 16.0;
  const float fScale = 3.0 / 65536.0;
  const float iScale = 3.0 * 256.0;

  // Per-component integer offset to bit representation of FP32 position
  int3 iOff = int3(N * iScale);

  // Select per-component between small fixed offset or variable offset
  // depending on distance to origin
  float3 iPos = asfloat(asint(X) + select(X < 0.0, -iOff, iOff));
  float3 fOff = N * fScale;

  return select(abs(X) < origin, X + fOff, iPos);
}

#define RT_MATERIAL_INDEX_BIT_OFFSET 8
#define RT_MATERIAL_TYPE_MASK 0xff
struct RTHitInfo {
  float3 x;
  float3 x_prev;
  float3 v;
  float3 t;
  float3 n;
  float2 uv;
  float mip;
  float tmin;
  uint instance_id;
  uint geometry_idx;
  uint material_type_and_id;
  uint flags;

  bool IsSky() { return tmin == INF; }
  float3 GetXOffset() { return _GetXoffset(x, n); }
  bool IsTransparent() { return (flags & Moer::RTVM_TRANSPARANT) != 0; }

  uint GetMaterialType() {
    return material_type_and_id & RT_MATERIAL_TYPE_MASK;
  }

  uint GetMaterialID() {
    return material_type_and_id >> RT_MATERIAL_INDEX_BIT_OFFSET;
  }
};

#pragma endregion

struct RTViewParam {
  float4x4 view2world;
  float4x4 world2view;
  float4 frustum;
  float2 near_far;
  uint2 rect;
  float2 inv_rect;
  float2 jitter;
  float3 dir;
  float orthomode;
};

struct RTMaterialProp {
  float3 l_direct; // unshadowed
  float3 l_emi;
  float3 n;
  float3 t;
  float3 base_color;
  float roughness;
  float metalness;
  float curvature;
};
namespace Moer {
#pragma region[ ReSTIR ]

namespace DI {}; // namespace DI
#pragma endregion
} // namespace Moer

namespace Raytracing {
#pragma region[ material ]

float2 GetConeAngleFromAngularRadius(float mip, float tan_con_angle) {
  // // In any case, we are limited by the output resolution
  tan_con_angle = max(tan_con_angle, rt_config.tan_pixel_angular_radius);

  return float2(mip, tan_con_angle);
}

float2 GetConeAngleFromRoughness(float mip, float roughness) {
  // return float2(mip, roughness);
  float con_angle = tan(ImportanceSampling::GetSpecularLobeHalfAngle(
      roughness)); // TODO:  * 0.33333?

  return GetConeAngleFromAngularRadius(mip, roughness);
}

float3 GetSamplingCoords(uint texture_handle, float2 uv, float mip, int mode) {
  float2 texSize;
  TextureHandle tex = TextureHandle(texture_handle);
  tex.GetTexture2D<float4>().GetDimensions(texSize.x, texSize.y);

  // Recalculate for the current texture
  float mip_num = log2(max(texSize.x, texSize.y));
  mip += mip_num - MAX_MIP;
  if (mode == MIP_VISIBILITY) {
    // We must avoid using lower mips because it can lead to significant
    // increase in AHS invocations. Mips lower than 128x128 are skipped!
    mip = min(mip, mip_num - 7.0);
  } else
    mip += rt_config.camera_origin_gmip_bias.w *
           (mode == MIP_LESS_SHARP ? 0.5 : 1.0);
  mip = clamp(mip, 0.0, mip_num - 1.0);

  // #if( USE_LOAD == 1 )
  //     mip = round( mip );
  // #endif

  texSize *= exp2(-mip);

  // // Uv coordinates
  // #if( USE_LOAD == 1 )
  //     uv = frac( uv ) * texSize;
  // #endif

  return float3(uv, mip);
}

float3 GetLoadCoords(uint texture_handle, float2 uv, float mip, int mode) {
  float2 texSize;
  TextureHandle tex = TextureHandle(texture_handle);
  tex.GetTexture2D<float4>().GetDimensions(texSize.x, texSize.y);

  // Recalculate for the current texture
  float mip_num = log2(max(texSize.x, texSize.y));
  mip += mip_num - MAX_MIP;
  if (mode == MIP_VISIBILITY) {
    // We must avoid using lower mips because it can lead to significant
    // increase in AHS invocations. Mips lower than 128x128 are skipped!
    mip = min(mip, mip_num - 7.0);
  } else
    mip += rt_config.camera_origin_gmip_bias.w *
           (mode == MIP_LESS_SHARP ? 0.5 : 1.0);
  mip = clamp(mip, 0.0, mip_num - 1.0);

  // #if( USE_LOAD == 1 )
  mip = round(mip);
  // #endif

  texSize *= exp2(-mip);

  // // Uv coordinates
  // #if( USE_LOAD == 1 )
  uv = frac(uv) * texSize;
  // #endif

  return float3(uv, mip);
}

#pragma endregion
float3 ReconstructViewPosition(float2 uv, float4 camera_frustum,
                               float depth = 1.f, float orthomode = 0.0f) {
  float3 p;
  p.xy = uv * camera_frustum.zw + camera_frustum.xy;
  p.xy *= depth * (1.f - abs(orthomode)) + orthomode;
  p.z = depth;
  return p;
}

// Taken out from NRD
float GetSpecMagicCurve(float roughness) {
  float f = 1.0 - exp2(-200.0 * roughness * roughness);
  f *= STL::Math::Pow01(roughness, 0.5);

  return f;
}

float EstimateDiffuseProbability(RTHitInfo hit_info, RTMaterialProp mat,
                                 bool use_magic_boost = false) {
  // IMPORTANT: can't be used for hair tracing, but applicable in other hair
  // related calculations

  float3 albedo, Rf0;
  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(mat.base_color, mat.metalness,
                                                  albedo, Rf0);

  float NoV = abs(dot(mat.n, hit_info.v));
  float3 Fenv = STL::BRDF::EnvironmentTerm_Rtg(Rf0, NoV, mat.roughness);

  float lumSpec = STL::Color::Luminance(Fenv);
  float lumDiff = STL::Color::Luminance(albedo * (1.0 - Fenv));

  float diffProb = lumDiff / (lumDiff + lumSpec + 1e-6);

  // Boost diffuse if roughness is high
  if (use_magic_boost)
    diffProb = lerp(diffProb, 1.0, GetSpecMagicCurve(mat.roughness));

  return diffProb < 0.005 ? 0.0 : diffProb;
}

float3 GetAmbientBRDF(RTHitInfo hit_info, RTMaterialProp mat,
                      bool approximate = false) {
  float3 albedo, Rf0;
  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(mat.base_color, mat.metalness,
                                                  albedo, Rf0);

  float3 f_env = Rf0;
  if (!approximate) {
    float nov = abs(dot(mat.n, hit_info.v));
    f_env = STL::BRDF::EnvironmentTerm_Rtg(Rf0, nov, mat.roughness);
  }

  float3 amb_BRDF = albedo * (1.0 - f_env) + f_env;
  amb_BRDF *= float(!hit_info.IsSky());

  return amb_BRDF;
}

float3 GetMotion(float3 x, float3 x_prev) {
  float3 motion = x_prev - x;

  float view_z = STL::Geometry::AffineTransform(rt_config.world2view, x).z;
  float2 sample_uv = STL::Geometry::GetScreenUv(rt_config.world2clip, x);

  float view_z_prev =
      STL::Geometry::AffineTransform(rt_config.world2view_prev, x_prev).z;
  float2 sample_uv_prev =
      STL::Geometry::GetScreenUv(rt_config.world2clip_prev, x_prev);

  // IMPORTANT: scaling to "pixel" unit significantly improves utilization of
  // FP16
  motion.xy = (sample_uv_prev - sample_uv) * rt_config.rect_size;

  // IMPORTANT: 2.5D motion is preferred over 3D motion due to imprecision
  // issues caused by FP16 rounding negative effects
  motion.z = view_z_prev - view_z;

  return motion;
}

float3 GetMotionWorld(float3 x, float3 x_prev) {
  float3 motion = x_prev - x;

  return motion;
}
} // namespace Raytracing

#endif