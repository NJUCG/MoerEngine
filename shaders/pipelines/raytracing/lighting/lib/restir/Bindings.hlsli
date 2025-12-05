#ifndef DI_BINDINGS_HLSLI
#define DI_BINDINGS_HLSLI

#ifndef DI_BINDING_SLOT
#define DI_BINDING_SLOT 0
#endif

#include <shared/ShaderParameters.h>

#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>
#include <core/math/Math.hlsli>
#include <pipelines/raytracing/passes/GBufferUtils.hlsli>
#include <shared/Geometry.h>
#include <shared/utils/MoerMath.hlsli>

#define USE_RAYQUERY 1

[[vk::binding(0, DI_BINDING_SLOT)]] RaytracingAccelerationStructure tlas;
[[vk::binding(1, DI_BINDING_SLOT)]] RaytracingAccelerationStructure prev_tlas;

[[vk::binding(
    2,
    DI_BINDING_SLOT)]] ConstantBuffer<Moer::ResampleConstants> resample_params;
[[vk::binding(3,
              DI_BINDING_SLOT)]] RWStructuredBuffer<Moer::DI::PackedReservoir>
    light_reservoirs;
[[vk::binding(4, DI_BINDING_SLOT)]] RWTexture2D<float4> rw_diffuse_lighting;
[[vk::binding(5, DI_BINDING_SLOT)]] RWTexture2D<float4> rw_specular_lighting;
[[vk::binding(6, DI_BINDING_SLOT)]] RWTexture2D<int2> rw_temporal_sample_pos;
[[vk::binding(7, DI_BINDING_SLOT)]] RWTexture2DArray<float4> rw_gradients;
[[vk::binding(8, DI_BINDING_SLOT)]] RWTexture2D<float2> rw_restir_luminance;

[[vk::binding(9,
              DI_BINDING_SLOT)]] RWTexture2D<float4> rw_diffuse_lighting_prev;

[[vk::binding(10, DI_BINDING_SLOT)]] RWBuffer<uint2> rw_ris_buffer;
[[vk::binding(11, DI_BINDING_SLOT)]] RWBuffer<uint4> rw_ris_light_data_buffer;
[[vk::binding(12, DI_BINDING_SLOT)]] Buffer<float2> neighbor_offset_buf;

BINDLESS_BINDINGS(3, 2, 4, 5)
#include <materials/Material.hlsl>
#include <pipelines/raytracing/lighting/common/PolymorphicLight.hlsli>

#include <pipelines/raytracing/inline/RaytracingCommon.hlsli>

#ifndef DI_LIGHT_RESERVOIR_BUFFER
#define DI_LIGHT_RESERVOIR_BUFFER light_reservoirs
#endif

namespace Moer {

typedef Math::Rng::Hash RandomState;

float DiffuseTerm(float3 _v, float3 _n, float3 _l, float _roughness) {
  float3 h = normalize(_v + _l);
  float nol = saturate(dot(_n, _l));
  float nov = saturate(dot(_n, _v));
  float voh = saturate(dot(_v, h));

  // use lambert here to reduce the cost
  return STL::BRDF::DiffuseTerm(_roughness, nol, nov, voh) * max(nol, 0.f);
}

float3 SpecularTerm(float3 _v, float3 _l, float3 _n, float _roughness,
                    float3 _f0, out float3 _fresnel) {
  float3 h = normalize(_v + _l);
  float noh = saturate(dot(_n, h));
  float nov = saturate(dot(_n, _v));
  float voh = saturate(dot(_v, h));
  float nol = saturate(dot(_n, _l));

  if (nol <= 0.f)
    return 0.f;

  _fresnel = STL::BRDF::FresnelTerm(_f0, voh);
  float ndf = STL::BRDF::DistributionTerm(_roughness, noh);
  float g = STL::BRDF::GeometryTermMod(_roughness, nol, nov, voh, noh);
  float3 spec_term = _fresnel * ndf * g * nol;
  return spec_term;
}

struct LightSample {
  float3 x;
  float3 n;
  float3 radiance;
  float solid_angle_pdf;
  EPolyLightType type;

  static LightSample EmptyLightSample() {
    LightSample s = (LightSample)0;
    return s;
  }

  bool IsAnalytic() {
    return type != EPolyLightType::ELTriangle && type != EPolyLightType::ELEnv;
  }

  float SolidAnglePdf() { return solid_angle_pdf; }
};

struct Surface {
  float3 x;
  float3 v;
  float v_z;
  float3 n;
  float3 diffuse_albedo;
  float3 specular_f0;
  float roughness;
  float diffuse_prob;

  float GetDiffuseProbability() {
    float3 fresnel = STL::BRDF::FresnelTerm_Schlick(specular_f0, (dot(v, n)));
    float specular_weight = STL::Color::Luminance(fresnel);
    float diffuse_weight =
        STL::Color::Luminance(diffuse_albedo * (1.f - fresnel));

    float sum_weight = diffuse_weight + specular_weight;
    return sum_weight < 1e-6f ? 1.f : diffuse_weight / sum_weight;
  }

  bool IsValid() { return v_z != FP16_MAX; }

  float3 GetWorldPos() { return x; }

  float3 GetNormal() { return n; }

  float GetLinearDepth() { return v_z; }

  static Surface EmptySurface() {
    Surface s = (Surface)0;
    s.v_z = FP16_MAX;
    return s;
  }

  float3 WorldToTangent(float3 _w) {
    float3 t, b;
    Math::BranchlessONB(n, t, b);
    return float3(dot(b, _w), dot(t, _w), dot(n, _w));
  }

  float3 TangentToWorld(float3 _h) {
    float3 t, b;
    Math::BranchlessONB(n, t, b);
    return b * _h.x + t * _h.y + n * _h.z;
  }

  bool GetBrdfSample(out float3 _dir, inout RandomState _rng) {
    float3 rnd;
    rnd.x = _rng.GetFloat();
    rnd.y = _rng.GetFloat();
    rnd.z = _rng.GetFloat();

    if (rnd.z < diffuse_prob) {
      float pdf;
      float3 h = Math::SampleHemisphereCosineWithPdf(rnd.xy, pdf);
      _dir = TangentToWorld(h);
    } else {
      // specular
      float3 ve = normalize(WorldToTangent(v));
      float3 h = STL::ImportanceSampling::VNDF::GetRay(
          rnd.xy, float2(roughness, roughness), ve);
      h = normalize(h);
      _dir = reflect(-v, TangentToWorld(h));
    }
    return dot(_dir, n) > 0.f;
  }

  float GetBrdfPdf(float3 _dir) {
    float cos_theta = saturate(dot(_dir, n));

    float3 h = normalize(_dir + v);
    float noh = saturate(dot(n, h));
    float nov = saturate(dot(n, v));

    float voh = saturate(dot(v, h));
    float nol = saturate(dot(n, _dir));

    float diffuse_pdf = cos_theta / PI;
    float specular_pdf =
        STL::ImportanceSampling::VNDF::GetPDF(nov, noh, roughness);

    return cos_theta > 0.f ? lerp(specular_pdf, diffuse_pdf, diffuse_prob)
                           : 0.f;
  }

  float GetLightSampleTargetPdf(LightSample _sample) {
    if (_sample.solid_angle_pdf <= 0.f)
      return 0.f;

    float3 l = normalize(_sample.x - x);
    if (dot(l, n) <= 0.f)
      return 0.f;

    float d = DiffuseTerm(v, n, l, roughness);
    float3 fresnel;
    float3 s;

    s = SpecularTerm(v, l, n, roughness, specular_f0, fresnel);
    if (roughness == 0.f) {
      // perfect reflection, diffuse term is 0
      // d = 0.f;
    }
    float3 reflect_radiance =
        ((1.f - fresnel) * d * diffuse_albedo + s) * _sample.radiance;
    return STL::Color::Luminance(reflect_radiance) / _sample.solid_angle_pdf;
  }

  float GetLightTargetPdfForVolume(PolymorphicLightInfo _light,
                                   float3 _vol_center, float _vol_radius) {
    return PolymorphicLight::GetVolumeWeight(_light, _vol_center, _vol_radius);
  }

  LightSample SamplePolymorphicLight(PolymorphicLightInfo _light, float2 _uv) {
    PolymorphicLightSample light_sample =
        PolymorphicLight::Sample(_light, _uv, x);
    LightSample res;
    res.x = light_sample.pos;
    res.n = light_sample.normal;
    res.radiance = light_sample.radiance;
    res.solid_angle_pdf = light_sample.solid_angle_pdf;
    res.type = GetLightType(_light);

    return res;
  }

  void GetLightDirAndDist(LightSample _light, out float3 _dir,
                          out float _distance) {

    if (_light.type == EPolyLightType::ELEnv) {
      _dir = -_light.n;
      _distance = s_light_max_distance;
    } else {
      float3 l = _light.x - x;
      _distance = length(l);
      _dir = l / _distance;
    }
  }

  float3 Xoffset(float3 X, float3 N) {
    // TODO: try out:
    // https://developer.nvidia.com/blog/solving-self-intersection-artifacts-in-directx-raytracing/

    // RT Gems "A Fast and Robust Method for Avoiding Self-Intersection" (
    // updated version taken from Falcor ) Moves the ray origin further from
    // surface to prevent self-intersections, minimizes the distance.
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
  RayDesc SetupVisibilityRay(float3 _sample_pos, float _x_offset = 0.01f) {

    float3 start_point = x;
    float3 l = _sample_pos - start_point;

    RayDesc ray;
    ray.Origin = start_point;
    ray.Direction = normalize(l);
    ray.TMin = _x_offset;
    ray.TMax = max(_x_offset, length(l) - 2 * _x_offset);
    return ray;
  }

  void EvalBrdf(float3 _sample_pos, out float3 _diffuse, out float3 _specular) {
    float3 l = _sample_pos - x;
    _diffuse = DiffuseTerm(v, n, normalize(l), roughness);
    float3 fresnel = 0.f;
    _specular =
        SpecularTerm(v, normalize(l), n, roughness, specular_f0, fresnel);
    _diffuse *= (1.f - fresnel);
  }

  bool HasSimilarMaterial(Surface _other) {

    const float roughness_threshold = 0.15f;
    const float reflectance_threshold = 0.25f;
    const float albedo_threshold = 0.25f;
    // if (_other.roughness * roughness == 0.f) {
    //   return false;
    // }

    if (!Math::CompareDifferance(roughness, _other.roughness,
                                 roughness_threshold))
      return false;

    if (abs(STL::Color::Luminance(diffuse_albedo) -
            STL::Color::Luminance(_other.diffuse_albedo)) > albedo_threshold)
      return false;

    if (abs(STL::Color::Luminance(specular_f0) -
            STL::Color::Luminance(_other.specular_f0)) > reflectance_threshold)
      return false;

    return true;
  }
};

// prev to current & current to prev
int GetMappedLightIndex(int _cur_light_idx) {
  ArrayBuffer light_idx_buf =
      ArrayBuffer(resample_params.bindless_handles.light_index);

  uint mapped = light_idx_buf.Load<uint>(_cur_light_idx);
  return int(mapped) - 1;
}

Surface GetGBufferSurface(int2 _pixel_pos, ViewParam _view_param,
                          Texture2D<float> _gbuffer_depth,
                          Texture2D<uint> _gbuffer_normal,
                          Texture2D<uint> _gbuffer_diffuse_albedo,
                          Texture2D<uint> _gbuffer_specular_roughness) {
  Surface s = Surface::EmptySurface();
  if (any(_pixel_pos >= int2(_view_param.rect)))
    return s;
  s.v_z = _gbuffer_depth[_pixel_pos];

  if (s.v_z == FP16_MAX)
    return s;

  s.n = Math::OctToNdirUnorm32(_gbuffer_normal[_pixel_pos]);
  s.diffuse_albedo =
      Moer::Unpack_R11G11B10_UFLOAT(_gbuffer_diffuse_albedo[_pixel_pos]);
  float4 specular_roughness = Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(
      _gbuffer_specular_roughness[_pixel_pos]);
  s.specular_f0 = specular_roughness.xyz;
  s.roughness = specular_roughness.w;
  s.x = Moer::ViewdepthToWorldPos(_view_param, _pixel_pos, s.v_z);
  s.v = normalize(_view_param.dir_or_pos.xyz - s.x);
  s.diffuse_prob = s.GetDiffuseProbability();

  return s;
}

Surface GetGBufferSurface(int2 _pixel_pos, const bool _prev_frame = false) {

  TextureHandle gbuffer_depth;
  TextureHandle gbuffer_normal;
  TextureHandle gbuffer_diffuse_albedo;
  TextureHandle gbuffer_specular_roughness;
  if (_prev_frame) {
    gbuffer_depth =
        TextureHandle(resample_params.bindless_handles.gbuffer_prev_depth);
    gbuffer_normal =
        TextureHandle(resample_params.bindless_handles.gbuffer_prev_normal);
    gbuffer_diffuse_albedo = TextureHandle(
        resample_params.bindless_handles.gbuffer_prev_diffuse_albedo);
    gbuffer_specular_roughness = TextureHandle(
        resample_params.bindless_handles.gbuffer_prev_specular_roughness);

  } else {

    gbuffer_depth =
        TextureHandle(resample_params.bindless_handles.gbuffer_depth);
    gbuffer_normal =
        TextureHandle(resample_params.bindless_handles.gbuffer_normal);
    gbuffer_diffuse_albedo =
        TextureHandle(resample_params.bindless_handles.gbuffer_diffuse_albedo);
    gbuffer_specular_roughness = TextureHandle(
        resample_params.bindless_handles.gbuffer_specular_roughness);
  }

  Texture2D<float> gbuffer_depth_tex = gbuffer_depth.GetTexture2D<float>();
  Texture2D<uint> gbuffer_normal_tex = gbuffer_normal.GetTexture2D<uint>();
  Texture2D<uint> gbuffer_diffuse_albedo_tex =
      gbuffer_diffuse_albedo.GetTexture2D<uint>();
  Texture2D<uint> gbuffer_specular_roughness_tex =
      gbuffer_specular_roughness.GetTexture2D<uint>();

  return GetGBufferSurface(_pixel_pos, resample_params.main_view,
                           gbuffer_depth_tex, gbuffer_normal_tex,
                           gbuffer_diffuse_albedo_tex,
                           gbuffer_specular_roughness_tex);
}

float2 GetEnvironmentMapUVFromDir(float3 _dir) {
  float2 uv = Math::DirToEquirectangularUV(_dir);
  uv.x -= resample_params.scene_params.env_map_rotation;
  uv = frac(uv);
  return uv;
}

float EvalEnvMapPdf(float3 _dir) {
  if (!resample_params.restir_di_params.initial_sample_params.env_map_is) {
    return 1.f;
  }
  float2 uv = GetEnvironmentMapUVFromDir(_dir);
  uint2 pdf_tex_size = resample_params.env_pdf_size;
  uint2 texel_pos = uint2(uv * float2(pdf_tex_size));

  TextureHandle env_map_pdf =
      TextureHandle(resample_params.bindless_handles.env_pdf);
  Texture2D<float> env_map_pdf_tex = env_map_pdf.GetTexture2D<float>();
  float texel_val = env_map_pdf_tex[texel_pos];

  int last_mip = max(0, int(floor(log2(max(pdf_tex_size.x, pdf_tex_size.y)))));
  float avg_val = env_map_pdf_tex.mips[last_mip][uint2(0u, 0u)];

  float sum = avg_val * square(1u << last_mip);
  return texel_val / sum;
}

float EvalLocalLightSrcPdf(uint _light_idx) {
  uint2 pdf_tex_size = resample_params.local_light_pdf_size;
  uint2 texel_pos = Math::LinearIndexToZCurve(_light_idx);
  Texture2D<float> local_light_pdf_tex =
      TextureHandle(resample_params.bindless_handles.local_light_pdf)
          .GetTexture2D<float>();

  int last_mip = max(0, int(floor(log2(max(pdf_tex_size.x, pdf_tex_size.y)))));
  float avg_val = local_light_pdf_tex.mips[last_mip][uint2(0u, 0u)];

  float sum = avg_val * square(1u << last_mip);
  return local_light_pdf_tex[texel_pos] / sum;
}

PolymorphicLightInfo LoadLightInfo(uint _idx) {
  ArrayBuffer light_buffer =
      ArrayBuffer(resample_params.bindless_handles.poly_light_data);
  return light_buffer.Load<PolymorphicLightInfo>(_idx);
}

PolymorphicLightInfo LoadCompactLightInfo(uint _idx) {
  uint4 pack1;
  uint4 pack2;
  pack1 = rw_ris_light_data_buffer[_idx * 2];
  pack2 = rw_ris_light_data_buffer[_idx * 2 + 1];
  return UnpackCompactLightInfo(pack1, pack2);
}

bool StoreCompactLightInfo(uint _idx, PolymorphicLightInfo _info) {
  uint4 dat1;
  uint4 dat2;
  if (!PackCompactLightInfo(_info, dat1, dat2))
    return false;

  rw_ris_light_data_buffer[_idx * 2] = dat1;
  rw_ris_light_data_buffer[_idx * 2 + 1] = dat2;
  return true;
}

float3 GetEnvMapRadiance(float3 _dir) {
  if (!resample_params.scene_params.env_map_handle)
    return 0;

  TextureHandle tex_handle =
      TextureHandle(resample_params.scene_params.env_map_handle);

  float2 uv = Math::DirToEquirectangularUV(_dir);
  uv.x -= resample_params.scene_params.env_map_rotation;

  float3 env_radiance = tex_handle.SampleLevel<float4>(uv, 0).rgb;
  return env_radiance * resample_params.scene_params.env_map_scale;
}

uint GetLightIndex(uint _instance_idx, uint _geom_idx, uint _prim_idx) {
  uint light_idx = s_invalid_light_idx;

  ArrayBuffer inst_buf_arr =
      (ArrayBuffer)resample_params.bindless_handles.instance_data;

  ByteAddressBuffer inst_buf = inst_buf_arr.GetByteAddressBuffer();
  InstanceData instance =
      LoadInstanceData(inst_buf, _instance_idx * sizeof(InstanceData));
  uint geom_idx = instance.first_geom_idx + _geom_idx;
  ArrayBuffer geom_to_light_arr =
      (ArrayBuffer)resample_params.bindless_handles.geo_instance_to_light;
  light_idx = geom_to_light_arr.Load<uint>(geom_idx);
  if(_instance_idx == 5 && light_idx < 0)
  printf("instance_idx %d, geom_idx %d, prim_idx %d, light_idx %d\n",
         _instance_idx, _geom_idx, _prim_idx, light_idx);
  if (light_idx == s_invalid_light_idx)
    return light_idx;
  return light_idx + _prim_idx;
}

int2 ClampScreenPosition(int2 _pixel_pos) {
  int width = resample_params.main_view.rect.x;
  int height = resample_params.main_view.rect.y;

  if (_pixel_pos.x < 0)
    _pixel_pos.x = -_pixel_pos.x;
  if (_pixel_pos.y < 0)
    _pixel_pos.y = -_pixel_pos.y;
  if (_pixel_pos.x >= width)
    _pixel_pos.x = 2 * width - _pixel_pos.x - 1;
  if (_pixel_pos.y >= height)
    _pixel_pos.y = 2 * height - _pixel_pos.y - 1;

  return _pixel_pos;
}

bool RaytraceLocalLightVisibility(float3 _origin, float3 _direction,
                                  float _tmin, float _tmax, out uint _light_idx,
                                  out float2 _rand) {

  _light_idx = s_invalid_light_idx;
  _rand = 0.f;

  RayDesc ray;
  ray.Origin = _origin;
  ray.Direction = _direction;
  ray.TMin = _tmin;
  ray.TMax = _tmax;

  float2 uv;
  bool b_hit = false;

#if USE_RAYQUERY
  RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>
      ray_query;
  ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, RTVM_ALL, ray);
  ray_query.Proceed();

  b_hit = (ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT);

  if (b_hit) {
    _light_idx = GetLightIndex(ray_query.CommittedInstanceID(),
                               ray_query.CommittedGeometryIndex(),
                               ray_query.CommittedPrimitiveIndex());
    uv = ray_query.CommittedTriangleBarycentrics();
  }
#else
#endif

  if (_light_idx != s_invalid_light_idx)
    _rand = Math::BaryCentricsToRand2(Math::HitUVToBarycentrics(uv));

  return b_hit;
}

bool RaytraceConservativeVisibility(RaytracingAccelerationStructure _tlas,
                                    Surface _surface, float3 _sample_pos) {
  RayDesc ray = _surface.SetupVisibilityRay(_sample_pos);

  bool b_visible = false;
#if USE_RAYQUERY
  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
           RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
      ray_query;

  ray_query.TraceRayInline(_tlas, RAY_FLAG_NONE, RTVM_ALL, ray);
  ray_query.Proceed();

  b_visible = (ray_query.CommittedStatus() == COMMITTED_NOTHING);
#else
#endif

  return b_visible;
}

bool GetCurrentConservativeVisibility(Surface _surface, float3 _sample_pos) {
  return RaytraceConservativeVisibility(tlas, _surface, _sample_pos);
}

bool GetPreviousConservativeVisibility(Surface _surface, float3 _sample_pos) {
  if (!resample_params.enable_prev_tlas)
    return RaytraceConservativeVisibility(tlas, _surface, _sample_pos);
  else
    return RaytraceConservativeVisibility(prev_tlas, _surface, _sample_pos);
}

struct RayPayload {
  float3 throughput;
  float committed_ray_t;
  uint instance_id;
  uint geom_id;
  uint prim_id;
  float2 barycentrics;
  bool front_face;
};

bool EvalTransparentMaterial(uint _instance_id, uint _geom_id, uint _tri_id,
                             float2 _ray_barycentrics,
                             inout float3 _throughput) {
  ArrayBuffer instance_data_array =
      ArrayBuffer(resample_params.bindless_handles.instance_data);
  ArrayBuffer geom_data_array =
      ArrayBuffer(resample_params.bindless_handles.geom_data);
  ArrayBuffer material_data_array =
      ArrayBuffer(resample_params.bindless_handles.material_data);
  Moer::GeometryRecord geom = Moer::GetGeometryRecordFrom(
      _instance_id, _geom_id, _tri_id, _ray_barycentrics, Moer::EGA_UV,
      instance_data_array.GetByteAddressBuffer(),
      geom_data_array.GetByteAddressBuffer(),
      material_data_array.GetByteAddressBuffer());

  Moer::MaterialSample mat = Moer::SampleGeometryMaterial(
      geom, 0.f, 0.f, 0.f, Moer::EMA_BaseColor | Moer::EMA_Transmission);

  // bool alpha_masked = mat.opacity >= gs.material.alpha_cutoff;

  // if (gs.material.domain == MaterialDomain_AlphaTested)
  //     return alphaMask;

  // if (gs.material.domain == MaterialDomain_AlphaBlended)
  // {
  //     throughput *= (1.0 - ms.opacity);
  //     return false;
  // }

  // if (gs.material.domain == MaterialDomain_Transmissive ||
  //     (gs.material.domain == MaterialDomain_TransmissiveAlphaTested &&
  //     alphaMask) || gs.material.domain ==
  //     MaterialDomain_TransmissiveAlphaBlended)
  // {
  //     throughput *= ms.transmission;

  //     if (ms.hasMetalRoughParams)
  //         throughput *= (1.0 - ms.metalness) * ms.baseColor;

  //     if (gs.material.domain == MaterialDomain_TransmissiveAlphaBlended)
  //         throughput *= (1.0 - ms.opacity);

  //     return all(throughput == 0);
  // }
  _throughput *= (1.f - mat.opacity);
  return false;
}

float3 GetFinalVisibility(RaytracingAccelerationStructure _tlas,
                          Surface _surface, float3 _sample_pos) {
  RayDesc ray = _surface.SetupVisibilityRay(_sample_pos);

  uint instance_mask = RTVM_ALL;
  uint ray_flags = RAY_FLAG_NONE;

  RayPayload payload = (RayPayload)0;
  payload.instance_id = ~0u;
  payload.throughput = 1.f;

  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;

  ray_query.TraceRayInline(_tlas, ray_flags, instance_mask, ray);
  while (ray_query.Proceed()) {
    if (ray_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      if (EvalTransparentMaterial(ray_query.CandidateInstanceID(),
                                  ray_query.CandidateGeometryIndex(),
                                  ray_query.CandidatePrimitiveIndex(),
                                  ray_query.CandidateTriangleBarycentrics(),
                                  payload.throughput)) {
        ray_query.CommitNonOpaqueTriangleHit();
      }
    }
  }
  if (ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    payload.instance_id = ray_query.CommittedInstanceID();
    payload.geom_id = ray_query.CommittedGeometryIndex();
    payload.prim_id = ray_query.CommittedPrimitiveIndex();
    payload.barycentrics = ray_query.CommittedTriangleBarycentrics();
    payload.committed_ray_t = ray_query.CommittedRayT();
    payload.front_face = ray_query.CommittedTriangleFrontFace();
  }

  if (payload.instance_id == ~0u)
    return payload.throughput.xyz;
  else
    return 0.f;
}

void PermutationSampling(inout int2 _prev_pixel_pos, uint _rnd) {
  int2 offset = int2(_rnd & 3, (_rnd >> 2) & 3);
  _prev_pixel_pos += offset;

  _prev_pixel_pos.x ^= 3;
  _prev_pixel_pos.y ^= 3;

  _prev_pixel_pos -= offset;
}
} // namespace Moer

#endif