/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// NRD v4.9

// IMPORTANT: DO NOT MODIFY THIS FILE WITHOUT FULL RECOMPILATION OF NRD LIBRARY!

//=================================================================================================================================
// INPUT PARAMETERS
//=================================================================================================================================
/*
NON-NOISY INPUTS:
    float viewZ:
        - linear view space Z for primary rays ( linearized camera depth )

    float normal:
        - world-space normal

    float roughness:
        - "linear roughness" = sqrt( "m" ), where "m" = "alpha" - GGX roughness
        - usage: "isDiffuse ? 1.0 : roughness"

    float tanOfLightAngularRadius:
        - tan( lightAngularSize * 0.5 )
        - angular size is computed from the shadow receiving point
        - in other words, tanOfLightAngularRadius = lightRadius /
distanceToLight

NOISY INPUTS:
    float3 radiance:
        - radiance should not include material information ( use material
de-modulation to decouple materials )
        - radiance should not be premultiplied by "exposure"
        - for Primary Surface Replacements ( PSR ) throughput should be
de-modulated as much as possible ( see test 184 from the sample and
TraceOpaque.hlsl )
        - for diffuse rays
            - use COS-distribution ( or custom importance sampling )
            - if radiance is the result of path tracing, pass normalized hit
distance as the sum of 1-all hits (always ignore primary hit!)
        - for specular
            - use VNDF sampling ( or custom importance sampling )
                - most advanced v3 version:
https://gpuopen.com/download/publications/Bounded_VNDF_Sampling_for_Smith-GGX_Reflections.pdf
            - if radiance is the result of path tracing, pass hit distance for
the 1st bounce for the first time (always ignore primary hit!)

    float hitDist:
        - can't be negative
        - must not include primary hit distance
        - for the first bounce after the primary hit or PSR must be provided "as
is"
        - for susequent bounces must be adjusted by curvature and lobe energy
dissipation on the application side
        - must be explicitly set to 0 for rays pointing inside the surface (
better to nopt cast such rays )

    float normHitDist:
        - logically same as "hitDist", but normalized to [0; 1] range using
"REBLUR_FrontEnd_GetNormHitDist"
        - REBLUR must be aware of the normalization function via
"nrd::HitDistanceParameters"
        - by definition, normalized hit distance is AO ( ambient occlusion ) for
diffuse and SO ( specular occlusion ) for specular
        - AO can be used to emulate 2nd+ diffuse bounces
        - SO can be used to adjust IBL lighting
        - ".w" channel of diffuse / specular output is AO / SO
        - if you don't know which normalization function to choose use default
values of "nrd::HitDistanceParameters"

    float distanceToOccluder:
        - distance to occluder, must follow the rules:
            - NoL <= 0         - 0 ( it's very important )
            - NoL > 0 ( hit )  - hit distance
            - NoL > 0 ( miss ) - >= NRD_FP16_MAX
*/

#ifndef MOER_NRD_FRONTEND_HLSL
#define MOER_NRD_FRONTEND_HLSL

#include "MathLib/STL.hlsli"
#include "NRDEncoding.hlsli"

#include "NRDShared.h"

#pragma region[ helper functions ]

// Misc
float3 _NRD_SafeNormalize(float3 v) { return v * rsqrt(dot(v, v) + 1e-9); }

// Oct packing
float2 _NRD_EncodeUnitVector(float3 v, const bool bSigned) {
  v /= dot(abs(v), float3(1.0, 1.0, 1.0));

  float2 octWrap = (1.0 - abs(v.yx)) * (step(0.0, v.xy) * 2.0 - 1.0);
  v.xy = v.z >= 0.0 ? v.xy : octWrap;

  return bSigned ? v.xy : v.xy * 0.5 + 0.5;
}

float3 _NRD_DecodeUnitVector(float2 p, const bool bSigned,
                             const bool bNormalize) {
  p = bSigned ? p : (p * 2.0 - 1.0);

  // https://twitter.com/Stubbesaurus/status/937994790553227264
  float3 n = float3(p.xy, 1.0 - abs(p.x) - abs(p.y));
  float t = saturate(-n.z);
  n.xy -= t * (step(0.0, n.xy) * 2.0 - 1.0);

  return bNormalize ? normalize(n) : n;
}

// Color space
float _NRD_Luminance(float3 linearColor) {
  // IMPORTANT: must be in sync with ML_LUMINANCE_DEFAULT
  return dot(linearColor, float3(0.2126, 0.7152, 0.0722));
}

float3 _NRD_LinearToYCoCg(float3 color) {
  float Y = dot(color, float3(0.25, 0.5, 0.25));
  float Co = dot(color, float3(0.5, 0.0, -0.5));
  float Cg = dot(color, float3(-0.25, 0.5, -0.25));

  return float3(Y, Co, Cg);
}

float3 _NRD_YCoCgToLinear(float3 color) {
  float t = color.x - color.z;

  float3 r;
  r.y = color.x + color.z;
  r.x = t + color.y;
  r.z = t - color.y;

  return max(r, 0.0);
}

float3 _NRD_YCoCgToLinear_Corrected(float Y, float Y0, float2 CoCg) {
  Y = max(Y, 0.0);
  CoCg *= (Y + NRD_EPS) / (Y0 + NRD_EPS);

  return _NRD_YCoCgToLinear(float3(Y, CoCg));
}

// GGX dominant direction
float _NRD_GetSpecularDominantFactor(float NoV, float roughness) {
  float a = 0.298475 * log(39.4115 - 39.0029 * roughness);
  float dominantFactor = pow(saturate(1.0 - NoV), 10.8649) * (1.0 - a) + a;

  return saturate(dominantFactor);
}

float3 _NRD_GetSpecularDominantDirection(float3 N, float3 V,
                                         float dominantFactor) {
  float3 R = reflect(-V, N);
  float3 D = lerp(N, R, dominantFactor);

  return normalize(D);
}

float _NRD_GetSpecMagicCurve(float roughness) {
  return 1.0 - exp2(-30.0 * roughness * roughness);
}

// BRDF
float _NRD_Pow5(float x) { return pow(saturate(1.0 - x), 5.0); }

float _NRD_FresnelTerm(float Rf0, float VoNH) {
  return Rf0 + (1.0 - Rf0) * _NRD_Pow5(VoNH);
}

float _NRD_DistributionTerm(float roughness, float NoH) {
  float m = roughness * roughness;
  float m2 = m * m;

  float t = (NoH * m2 - NoH) * NoH + 1.0;
  float a = m / t;
  float d = a * a;

  return d / NRD_PI;
}

float _NRD_GeometryTerm(float roughness, float NoL, float NoV) {
  float m = roughness * roughness;
  float m2 = m * m;

  float a = NoL + sqrt(saturate((NoL - m2 * NoL) * NoL + m2));
  float b = NoV + sqrt(saturate((NoV - m2 * NoV) * NoV + m2));

  return 1.0 / max(a * b, NRD_EPS);
}

float _NRD_DiffuseTerm(float roughness, float NoL, float NoV, float VoH) {
  float m = roughness * roughness;

  float f = 2.0 * VoH * VoH * m - 0.5;
  float FdV = f * _NRD_Pow5(NoV) + 1.0;
  float FdL = f * _NRD_Pow5(NoL) + 1.0;
  float d = FdV * FdL;

  return d / NRD_PI;
}

float2 _NRD_ComputeBrdfs(float3 Ld, float3 Ls, float3 N, float3 V, float Rf0,
                         float roughness) {
  float2 result;
  float NoV = abs(dot(N, V));

  // Diffuse
  {
    float3 H = normalize(Ld + V);

    float NoL = saturate(dot(N, Ld));
    float VoH = saturate(dot(V, H));

    float F = _NRD_FresnelTerm(Rf0, VoH);
    float Kdiff = _NRD_DiffuseTerm(roughness, NoL, NoV, VoH);

    result.x = (1.0 - F) * Kdiff * NoL;
  }

  // Specular
  {
    float3 H = normalize(Ls + V);
    H = normalize(lerp(N, H, roughness)); // Fixed H // TODO: roughness => smc?

    float NoL = saturate(dot(N, Ls));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    float F = _NRD_FresnelTerm(Rf0, VoH);
    float D = _NRD_DistributionTerm(roughness, NoH);
    float G = _NRD_GeometryTerm(roughness, NoL, NoV);

    result.y = F * D * G * NoL;
  }

  return result;
}

// Hit distance normalization
float _REBLUR_GetHitDistanceNormalization(float viewZ, float4 hitDistParams,
                                          float roughness) {
  return (hitDistParams.x + abs(viewZ) * hitDistParams.y) *
         lerp(1.0, hitDistParams.z,
              saturate(exp2(hitDistParams.w * roughness * roughness)));
}

// Is valid?
bool _NRD_IsInvalid(float3 x) { return any(isnan(x)) || any(isinf(x)); }

bool _NRD_IsInvalid(float x) { return isnan(x) || isinf(x); }

#pragma endregion

#pragma region[ resource encode&decode ]

//=================================================================================================================================
// FRONT-END - GENERAL
//=================================================================================================================================

// This function is used in all denoisers to decode normal, roughness and
// optional materialID IN_NORMAL_ROUGHNESS => X
float4 NRD_FrontEnd_UnpackNormalAndRoughness(float4 p, out float materialID) {
  float4 r;
#if (NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_R10G10B10A2_UNORM)
  r.xyz = _NRD_DecodeUnitVector(p.xy, false, false);
  r.w = p.z;

  materialID = p.w * 3.0;
#else
#if (NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA8_UNORM ||                 \
     NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA16_UNORM)
  p.xyz = p.xyz * 2.0 - 1.0;
#endif

  r.xyz = p.xyz;
  r.w = p.w;

  materialID = 0.0;
#endif

  r.xyz = _NRD_SafeNormalize(r.xyz);

#if (NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQRT_LINEAR)
  r.w *= r.w;
#elif (NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQ_LINEAR)
  r.w = sqrt(saturate(r.w));
#endif

  return r;
}

// IN_NORMAL_ROUGHNESS => X
float4 NRD_FrontEnd_UnpackNormalAndRoughness(float4 p) {
  float unused;

  return NRD_FrontEnd_UnpackNormalAndRoughness(p, unused);
}

// Not used in NRD
// X => IN_NORMAL_ROUGHNESS
float4 NRD_FrontEnd_PackNormalAndRoughness(float3 N, float roughness,
                                           float materialID) {
  float4 p;

#if (NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQRT_LINEAR)
  roughness = sqrt(saturate(roughness));
#elif (NRD_ROUGHNESS_ENCODING == NRD_ROUGHNESS_ENCODING_SQ_LINEAR)
  roughness *= roughness;
#endif

#if (NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_R10G10B10A2_UNORM)
  p.xy = _NRD_EncodeUnitVector(N, false);
  p.z = roughness;
  p.w = saturate(materialID / 3.0);
#else
  // Best fit ( optional )
  N /= max(abs(N.x), max(abs(N.y), abs(N.z)));

#if (NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA8_UNORM ||                 \
     NRD_NORMAL_ENCODING == NRD_NORMAL_ENCODING_RGBA16_UNORM)
  N = N * 0.5 + 0.5;
#endif

  p.xyz = N;
  p.w = roughness;
#endif

  return p;
}

//=================================================================================================================================
// FRONT-END - SPECULAR HIT DISTANCE AVERAGING ( in case of rpp > 1 )
//=================================================================================================================================

float NRD_FrontEnd_SpecHitDistAveraging_Begin() { return NRD_INF; }

float NRD_FrontEnd_TrimHitDistance(float hitDist, float threshold) // optional
{
  // Sampling can produce rays pointing inside the surface, leading to "hitDist
  // = 0". But due to ray offsetting actual "hitDist" can be a very small value
  // in this case. Since NRD has been designed to handle "hitDist = 0" case,
  // accidentally small "hitDist" values better trim to 0

  return hitDist < threshold ? 0.0 : hitDist;
}

void NRD_FrontEnd_SpecHitDistAveraging_Add(inout float accumulatedSpecHitDist,
                                           float hitDist) {
  // TODO: for high roughness it can be blended to average
  accumulatedSpecHitDist =
      min(accumulatedSpecHitDist, hitDist == 0.0 ? NRD_INF : hitDist);
}

void NRD_FrontEnd_SpecHitDistAveraging_End(inout float accumulatedSpecHitDist) {
  accumulatedSpecHitDist =
      accumulatedSpecHitDist == NRD_INF ? 0.0 : accumulatedSpecHitDist;
}

//=================================================================================================================================
// FRONT-END - REBLUR
//=================================================================================================================================

// This function returns AO / SO which REBLUR can decode back to "hit distance"
// internally
float REBLUR_FrontEnd_GetNormHitDist(float hitDist, float viewZ,
                                     float4 hitDistParams, float roughness) {
  float f =
      _REBLUR_GetHitDistanceNormalization(viewZ, hitDistParams, roughness);

  return saturate(hitDist / f);
}

// X => IN_DIFF_RADIANCE_HITDIST
// X => IN_SPEC_RADIANCE_HITDIST
// normHitDist must be packed by "REBLUR_FrontEnd_GetNormHitDist"
float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3 radiance,
                                                  float normHitDist,
                                                  bool sanitize) {
  if (sanitize) {
    radiance = _NRD_IsInvalid(radiance) ? float3(0, 0, 0)
                                        : clamp(radiance, 0, NRD_FP16_MAX);
    normHitDist = _NRD_IsInvalid(normHitDist) ? 0 : saturate(normHitDist);
  }

  radiance = _NRD_LinearToYCoCg(radiance);

  return float4(radiance, normHitDist);
}

// X => IN_DIFF_SH0 and IN_DIFF_SH1
// X => IN_SPEC_SH0 and IN_SPEC_SH1
// normHitDist must be packed by "REBLUR_FrontEnd_GetNormHitDist"
float4 REBLUR_FrontEnd_PackSh(float3 radiance, float normHitDist,
                              float3 direction, out float4 out1,
                              bool sanitize) {
  if (sanitize) {
    radiance = _NRD_IsInvalid(radiance) ? float3(0, 0, 0)
                                        : clamp(radiance, 0, NRD_FP16_MAX);
    normHitDist = _NRD_IsInvalid(normHitDist) ? 0 : saturate(normHitDist);
    direction = _NRD_IsInvalid(direction) ? float3(0, 0, 0)
                                          : clamp(direction, -1.0, 1.0);
  }

  NRD_SG sg = _NRD_SG_Create(radiance, direction, normHitDist);

  // IN_DIFF_SH0 / IN_SPEC_SH0
  float4 out0 = float4(sg.c0, sg.chroma, sg.normHitDist);

  // IN_DIFF_SH1 / IN_SPEC_SH1
  out1 = float4(sg.c1, sg.sharpness);

  return out0;
}

// X => IN_DIFF_DIRECTION_HITDIST
// normHitDist must be packed by "REBLUR_FrontEnd_GetNormHitDist"
float4 REBLUR_FrontEnd_PackDirectionalOcclusion(float3 direction,
                                                float normHitDist,
                                                bool sanitize) {
  if (sanitize) {
    direction = _NRD_IsInvalid(direction) ? float3(0, 0, 0)
                                          : clamp(direction, -1.0, 1.0);
    normHitDist = _NRD_IsInvalid(normHitDist) ? 0 : saturate(normHitDist);
  }

  NRD_SG sg = _NRD_SG_Create(normHitDist.xxx, direction, normHitDist);

  return float4(sg.c1, sg.c0);
}

//=================================================================================================================================
// FRONT-END - RELAX
//=================================================================================================================================

// X => IN_DIFF_RADIANCE_HITDIST
// X => IN_SPEC_RADIANCE_HITDIST
float4 RELAX_FrontEnd_PackRadianceAndHitDist(float3 radiance, float hitDist,
                                             bool sanitize) {
  if (sanitize) {
    radiance = _NRD_IsInvalid(radiance) ? float3(0, 0, 0)
                                        : clamp(radiance, 0, NRD_FP16_MAX);
    hitDist = _NRD_IsInvalid(hitDist) ? 0 : clamp(hitDist, 0, NRD_FP16_MAX);
  }

  return float4(radiance, hitDist);
}

// X => IN_DIFF_SH0 and IN_DIFF_SH1
// X => IN_SPEC_SH0 and IN_SPEC_SH1
float4 RELAX_FrontEnd_PackSh(float3 radiance, float hitDist, float3 direction,
                             out float4 out1, bool sanitize) {
  if (sanitize) {
    radiance = _NRD_IsInvalid(radiance) ? float3(0, 0, 0)
                                        : clamp(radiance, 0, NRD_FP16_MAX);
    hitDist = _NRD_IsInvalid(hitDist) ? 0 : clamp(hitDist, 0, NRD_FP16_MAX);
    direction = _NRD_IsInvalid(direction) ? float3(0, 0, 0)
                                          : clamp(direction, -1.0, 1.0);
  }

  // IN_DIFF_SH0 / IN_SPEC_SH0
  float4 out0 = float4(radiance, hitDist);

  // IN_DIFF_SH1 / IN_SPEC_SH1
  out1 = float4(direction * _NRD_Luminance(radiance), 0);

  return out0;
}

//=================================================================================================================================
// FRONT-END - SIGMA
//=================================================================================================================================

// SIGMA single light

// Infinite ( directional ) light source
// X => IN_PENUMBRA
float SIGMA_FrontEnd_PackPenumbra(float distanceToOccluder,
                                  float tanOfLightAngularRadius) {
  float penumbraSize = distanceToOccluder * tanOfLightAngularRadius;
  float penumbraRadius = penumbraSize * 0.5;

  return distanceToOccluder >= NRD_FP16_MAX ? NRD_FP16_MAX
                                            : min(penumbraRadius, 32768.0);
}

// Local light source
// X => IN_PENUMBRA
// "lightSize" must be an acceptable projection to the plane perpendicular to
// the light direction
float SIGMA_FrontEnd_PackPenumbra(float distanceToOccluder,
                                  float distanceToLight, float lightSize) {
  float penumbraSize = lightSize * distanceToOccluder /
                       max(distanceToLight - distanceToOccluder, NRD_EPS);
  float penumbraRadius = penumbraSize * 0.5;

  return distanceToOccluder >= NRD_FP16_MAX ? NRD_FP16_MAX
                                            : min(penumbraRadius, 32768.0);
}

// X => IN_TRANSLUCENCY
float4 SIGMA_FrontEnd_PackTranslucency(float distanceToOccluder,
                                       float3 translucency) {
  float4 r;
  r.x = float(distanceToOccluder >= NRD_FP16_MAX);
  r.yzw = saturate(translucency);

  return r;
}

#pragma endregion

#endif