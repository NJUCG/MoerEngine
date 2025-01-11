#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
RTCONFIG_BINDING(1, 0);
BINDLESS_BINDINGS(3, 2, 4, 5);
#include <framework/Lighting.hlsl>

#include <framework/RaytracingShared.hlsli>

#include <framework/Material.hlsl>
#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/Packing.h>

struct Param {
  uint instance_buffer_handle;
  uint geometry_buffer_handle;
  uint material_buffer_handle;
  uint global_param_handle;
  uint light_buffer_handle;
  uint2 rect;
  float2 inv_rect;
  float2 jitter;
  uint frame_index;
};

[[vk::push_constant]] ConstantBuffer<Param> param;
[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas
    : register(t0, space0);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_normal_roughness
    : register(u0, space1);
[[vk::binding(1, 1)]] RWTexture2D<float4> out_basecolor_metalness
    : register(u1, space1);
[[vk::binding(2, 1)]] RWTexture2D<float3> out_direct_lighting
    : register(u3, space1);
[[vk::binding(3, 1)]] RWTexture2D<float3> out_emission : register(u4, space1);
[[vk::binding(4, 1)]] RWTexture2D<float4> out_diffuse;
[[vk::binding(5, 1)]] RWTexture2D<float4> out_specular;
[[vk::binding(6, 1)]] RWTexture2D<float> out_view_z;
[[vk::binding(7, 1)]] RWTexture2D<float3> out_mv;
[[vk::binding(8, 1)]] RWTexture2D<float2> out_shadow_info;

#define SKY_INTENSITY 1.0
#define SUN_INTENSITY 8.0

struct PathTracingDesc {
  RTHitInfo hit_info;
  RTMaterialProp mat;
  uint2 pixel_pos;
  uint path_num;
  uint bounce_num;
  uint instance_mask;
  uint ray_flags;
};

struct PathTracingResult {
  float3 diffuse_radiance;
  float diffuse_hit_dist;
  float3 specular_radiance;
  float specular_hit_dist;
};

float3 GetSunIntensity(float3 v, float3 sunDirection, float tanAngularRadius) {
  float b = dot(v, sunDirection);
  float d = length(v - sunDirection * b);

  float glow = saturate(1.015 - d);
  glow *= b * 0.5 + 0.5;
  glow *= 0.6;

  float a = STL::Math::Sqrt01(1.0 - b * b) / b;
  float sun = 1.0 - STL::Math::SmoothStep(tanAngularRadius * 0.9,
                                          tanAngularRadius * 1.66, a);
  sun *= float(b > 0.0);
  sun *= 1.0 - STL::Math::Pow01(1.0 - v.z, 4.85);
  sun *= STL::Math::SmoothStep(0.0, 0.1, sunDirection.z);
  sun += glow;

  float3 sunColor = lerp(float3(1.0, 0.6, 0.3), float3(1.0, 0.9, 0.7),
                         STL::Math::Sqrt01(sunDirection.z));
  sunColor *= saturate(sun);

  sunColor *= STL::Math::SmoothStep(-0.01, 0.05, sunDirection.z);

  return STL::Color::GammaToLinear(sunColor) * SUN_INTENSITY;
}

float3 GetSkyIntensity(float3 v, float3 sunDirection, float tanAngularRadius) {
  float atmosphere = sqrt(1.0 - saturate(v.z));

  float scatter = pow(saturate(sunDirection.z), 1.0 / 15.0);
  scatter = 1.0 - clamp(scatter, 0.8, 1.0);

  float3 scatterColor =
      lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.3, 0.0) * 1.5, scatter);
  float3 skyColor =
      lerp(float3(0.2, 0.4, 0.8), float3(scatterColor), atmosphere / 1.3);
  skyColor *= saturate(1.0 + sunDirection.z);

  float ground = 0.5 + 0.5 * STL::Math::SmoothStep(-1.0, 0.0, v.z);
  skyColor *= ground;

  return STL::Color::GammaToLinear(saturate(skyColor)) * SKY_INTENSITY +
         GetSunIntensity(v, sunDirection, tanAngularRadius);
}

RTHitInfo CastRay(float3 origin, float3 direction, float tmin, float tmax,
                  float2 mip_and_cone, RaytracingAccelerationStructure accel,
                  uint instance_mask, uint ray_flags) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = tmin;
  ray.TMax = tmax;

  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;
  ray_query.TraceRayInline(accel, ray_flags, instance_mask, ray);

  ArrayBuffer instance_buffer = ArrayBuffer(param.instance_buffer_handle);
  ArrayBuffer geometry_buffer = ArrayBuffer(param.geometry_buffer_handle);

  //   ArrayBuffer primitive_buffer =
  //   ArrayBuffer(param.primitive_buffer_handle); ArrayBuffer vtx_buffer =
  //   ArrayBuffer(param.vtx_buffer_handle);

  while (ray_query.Proceed()) {
    if (ray_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      ray_query.CommitNonOpaqueTriangleHit();
    } else {
      ray_query.Abort();
      break;
    }
  }
  RTHitInfo hit_info = (RTHitInfo)0;

  if (ray_query.CommittedStatus() == COMMITTED_NOTHING) {
    hit_info.tmin = INF;
    hit_info.x = origin + direction * hit_info.tmin;
    hit_info.x_prev = hit_info.x;

  } else {
    hit_info.tmin = ray_query.CommittedRayT();
    hit_info.mip = mip_and_cone.x;
    // printf("mip %f\n", hit_info.mip);

    uint instance_id = ray_query.CommittedInstanceID();
    hit_info.instance_id = instance_id;
    hit_info.geometry_idx = ray_query.CommittedGeometryIndex();

    Moer::InstanceData instance_data =
        Moer::LoadInstanceData(instance_buffer.GetByteAddressBuffer(),
                               instance_id * sizeof(Moer::InstanceData));

    float3x4 model2world = ray_query.CommittedObjectToWorld3x4();

    uint glob_geom_id = instance_data.first_geom_idx + hit_info.geometry_idx;
    Moer::GeometryData geom_data =
        Moer::LoadGeometryData(geometry_buffer.GetByteAddressBuffer(),
                               glob_geom_id * sizeof(Moer::GeometryData));

    uint primitive_id = ray_query.CommittedPrimitiveIndex();

    ArrayBuffer idx_buffer = ArrayBuffer(geom_data.index_buffer_handle);
    uint3 indices =
        idx_buffer.Load<uint3>(primitive_id, geom_data.index_offset);

    ArrayBuffer vtx_buffer = ArrayBuffer(geom_data.vertex_buffer_handle);

    float3 barycentrics;
    barycentrics.yz = ray_query.CandidateTriangleBarycentrics();
    barycentrics.x = 1.0f - barycentrics.y - barycentrics.z;

    float3 positions[3];
    positions[0] = vtx_buffer.Load<float3>(indices.x, geom_data.vertex_offset);
    positions[1] = vtx_buffer.Load<float3>(indices.y, geom_data.vertex_offset);
    positions[2] = vtx_buffer.Load<float3>(indices.z, geom_data.vertex_offset);

    float3 pos = Moer::Interpolate(positions, barycentrics);

    float3 normals[3];
    normals[0] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.x, geom_data.normal_offset));
    normals[1] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.y, geom_data.normal_offset));
    normals[2] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.z, geom_data.normal_offset));

    float3 normal = Moer::Interpolate(normals, barycentrics);

    float3 tangents[3];
    tangents[0] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.x, geom_data.tangent_offset));
    tangents[1] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.y, geom_data.tangent_offset));
    tangents[2] = Moer::Unpack_RGB8_SNORM(
        vtx_buffer.Load<uint>(indices.z, geom_data.tangent_offset));

    float3 tangent = Moer::Interpolate(tangents, barycentrics);

    float2 uv0s[3];
    uv0s[0] = vtx_buffer.Load<float2>(indices.x, geom_data.texcoord0_offset);
    uv0s[1] = vtx_buffer.Load<float2>(indices.y, geom_data.texcoord0_offset);
    uv0s[2] = vtx_buffer.Load<float2>(indices.z, geom_data.texcoord0_offset);

    float2 uv0 = Moer::Interpolate(uv0s, barycentrics);

    // RTPrimitive primitive = primitive_buffer.Load<RTPrimitive>(primitive_id);
    // RTVertex vtx[3];
    // vtx[0] = vtx_buffer.Load<RTVertex>(primitive.indices.x +
    //                                    instance_data.vtx_offset);
    // vtx[1] = vtx_buffer.Load<RTVertex>(primitive.indices.y +
    //                                    instance_data.vtx_offset);
    // vtx[2] = vtx_buffer.Load<RTVertex>(primitive.indices.z +
    //                                    instance_data.vtx_offset);
    float3x3 model2worldrot = (float3x3)model2world;
    normal = mul(model2worldrot, normal).xyz;
    normal = normalize(normal);

    hit_info.n = normal;
    hit_info.uv = uv0;

    tangent = mul(model2worldrot, tangent).xyz;
    tangent = normalize(tangent);

    hit_info.t = tangent;

    float NoR = abs(dot(hit_info.n, direction));

    float a = hit_info.tmin * mip_and_cone.y;
    a *= 1.f / NoR;
    // a *= primitive.world_uv_units;
    // printf("a %f\n", a);

    float mip = log2(a);
    mip += MAX_MIP - 2;
    mip = max(0.f, mip);
    hit_info.mip += mip;

    hit_info.x = origin + direction * hit_info.tmin;
    hit_info.x_prev = hit_info.x;
    hit_info.flags = 0;
    hit_info.material_type_and_id = geom_data.mat_idx_and_type;
  }
  hit_info.v = -direction;
  return hit_info;
}

bool CastVisibilityRay(float3 origin, float3 direction, float tmin, float tmax,
                       float2 mip_and_cone,
                       RaytracingAccelerationStructure accel,
                       uint instance_mask, uint ray_flags) {
  RayDesc ray_desc;
  ray_desc.Origin = origin;
  ray_desc.Direction = direction;
  ray_desc.TMin = tmin;
  ray_desc.TMax = tmax;

  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
           RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
      ray_query;
  ray_query.TraceRayInline(accel, ray_flags, instance_mask, ray_desc);

  while (ray_query.Proceed()) {
    if (ray_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      ray_query.CommitNonOpaqueTriangleHit();
    } else {
      ray_query.Abort();
      break;
    }
  }

  return ray_query.CommittedStatus() == COMMITTED_NOTHING;
}

RTMaterialProp GetMaterialProps(in RTHitInfo hit_info) {
  RTMaterialProp mat = (RTMaterialProp)0;

  MaterialData mat_data = UnpackMaterialData<MaterialData>(
      param.material_buffer_handle, hit_info.GetMaterialID());

  float4 sun_direction_exposure = rt_config.sun_direction_gexposure;
  float3 c_sky = GetSkyIntensity(-hit_info.v, sun_direction_exposure.xyz,
                                 rt_config.tan_sun_angular_radius);

  [branch] if (hit_info.IsSky()) {
    mat.l_emi = c_sky;

    return mat;
  }
  // base color
  float4 color = mat_data.base_color_factor;
  //   printf("mat_data.albedo_map %d\n", mat_data.albedo_map);
  if (mat_data.albedo_map != 0) {
    TextureHandle albedo_map = TextureHandle(mat_data.albedo_map);
    //   printf("albedo_map %d material_buffer_handle %d \n",
    //   mat_data.albedo_map, param.material_buffer_handle);

    float3 coords = Raytracing::GetSamplingCoords(
        albedo_map.handle, hit_info.uv, hit_info.mip, MIP_SHARP);
    color = albedo_map.SampleLevel<float4>(coords.xy, coords.z);
  }
  //   printf("coord z %f\n", coords.z);

  //   uint tex_handle =
  //       g__array_114514_bdls[NonUniformResourceIndex(albedo_map.handle)];
  //   uint tex_idx = tex_handle >> 8;
  //   uint sampler_idx = tex_handle & 0xff;
  //   Texture2D<float4> tex = Texture2D<float4>(
  //       gTexture2Dfloat4__114514_bdls[NonUniformResourceIndex(
  //           tex_idx)]);

  //           SamplerState splr =
  //           SamplerState(gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)]);
  //                    float4 color = tex.SampleLevel(splr, coords.xy, 1.f);
  //   printf("sampler_idx %d\n", sampler_idx);
  //   printf("albedo_map.handle %d tex_handle %d tex_idx %d sampler_idx %d\n",
  //   albedo_map.handle, tex_handle, tex_idx, sampler_idx);
  float3 base_color = color.xyz;
  float3 n = hit_info.n;

  float roughness = mat_data.roughness_factor;
  float metalness = mat_data.metallic_factor;
  // metallic
  // if (mat_data.metallic_roughness_map != -1) {
  //   TextureHandle metallic_roughness_map =
  //       TextureHandle(mat_data.metallic_roughness_map);

  //   coords = Raytracing::GetSamplingCoords(
  //       metallic_roughness_map.handle, hit_info.uv, hit_info.mip, MIP_SHARP);
  //   float2 mr =
  //       metallic_roughness_map.SampleLevel2D<float4>(coords.xy, coords.z).xy;
  //   metalness = mr.x;
  //   roughness = mr.y;
  // }
  float3 l_emi = mat_data.emissive_factor;

  float emission_level = STL::Color::Luminance(l_emi);
  emission_level = saturate(emission_level * 50.0);

  metalness = lerp(metalness, 0.0, emission_level);
  roughness = lerp(roughness, 1.0, emission_level);

  // printf("metalness %f roughness %f\n", metalness, roughness);

  ArrayBuffer light_buffer = ArrayBuffer(param.light_buffer_handle);
  LightData sun_light = light_buffer.Load<LightData>(0);

  float3 l_direct = (float3)0;

  float NoL = saturate(dot(n, sun_direction_exposure.xyz));
  float shadow = STL::Math::SmoothStep(0.03, 0.1, NoL);

  [branch] if (shadow != 0.f) {
    float3 c_sun =
        GetSunIntensity(sun_direction_exposure.xyz, sun_direction_exposure.xyz,
                        rt_config.tan_sun_angular_radius);
    float3 c_imp =
        lerp(c_sky, c_sun, STL::Math::SmoothStep(0.0, 0.2, metalness));
    c_imp *= STL::Math::SmoothStep(0.01, 0.05, sun_direction_exposure.z);

    {
      float3 albedo, Rf0;
      STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(base_color.xyz, metalness,
                                                      albedo, Rf0);

      float3 c_diff, s_spec;
      STL::BRDF::DirectLighting(n, sun_direction_exposure.xyz, hit_info.v, Rf0,
                                roughness, c_diff, s_spec);
      l_direct = c_diff * albedo * c_sun + s_spec * c_imp;
    }
    l_direct *= shadow;
  }

  mat.l_direct = l_direct;
  mat.l_emi = l_emi;
  mat.n = n;
  mat.t = hit_info.t;
  mat.base_color = base_color;
  mat.roughness = roughness;
  mat.metalness = metalness;
  mat.curvature = 0.0f;

  return mat;
}

PathTracingResult PathTracing(PathTracingDesc pt_desc) {

  RTViewParam view =
      ArrayBuffer(param.global_param_handle).Load<RTViewParam>(0);

  float view_z = mul(float4(pt_desc.hit_info.x, 1.f), view.world2view).z;

  // Pathtracing from primary hit

  PathTracingResult pt_result = (PathTracingResult)0;

  uint path_num = pt_desc.path_num;
  uint diffuse_path_num = 0;

  [loop] for (uint i = 0; i < path_num; i++) {
    RTHitInfo hit_info = pt_desc.hit_info;
    RTMaterialProp mat = pt_desc.mat;

    float accumulate_hit_dist = 0.f;
    float accumulate_diffuse_like_motion = 0.f;

    float3 l_sum = 0.f;
    float3 path_throughput = 1.f;

    bool is_diffuse_path = false;
    {
      float diffuse_probablility =
          Raytracing::EstimateDiffuseProbability(hit_info, mat);

      float rnd = STL::Rng::Hash::GetFloat();

      is_diffuse_path = rnd < diffuse_probablility;
    }
    [loop] for (uint bounce = 1;
                bounce <= pt_desc.bounce_num && !hit_info.IsSky(); bounce++) {

      bool is_diffuse = is_diffuse_path;
      //   if(bounce > 3){
      //     printf("bounce %d\n", bounce);
      //   }
      // current point
      {
        float diffuse_probablility =
            Raytracing::EstimateDiffuseProbability(hit_info, mat);

        float rnd = STL::Rng::Hash::GetFloat();

        if (bounce > 1) {
          is_diffuse = rnd < diffuse_probablility;
          path_throughput *= abs(float(!is_diffuse) - diffuse_probablility);
        }
        if (bounce == 1) {
          is_diffuse_path = is_diffuse;
        }

        float2 mip_and_cone =
            Raytracing::GetConeAngleFromRoughness(hit_info.mip, mat.roughness);

        float3x3 local_basis = STL::Geometry::GetBasis(mat.n);

        float3 v_local = STL::Geometry::RotateVector(local_basis, hit_info.v);

        float3 ray = 0.f;
        uint samples_num = 0;

        uint max_sample_num = 0;

        if (bounce == 1) {
        }

        max_sample_num = max(1, max_sample_num);

        // mainly for hair in the future
        for (uint sample_idx = 0; sample_idx < max_sample_num; sample_idx++) {
          float2 rnd = STL::Rng::Hash::GetFloat2();

          float3 r;
          if (is_diffuse) {
            r = STL::ImportanceSampling::Cosine::GetRay(rnd);
          } else {
            float3 h_local = STL::ImportanceSampling::VNDF::GetRay(
                rnd, mat.roughness, v_local, 1.f);

            r = reflect(-v_local, h_local);
          }

          bool is_miss = r.z < 0.f;
          r = STL::Geometry::RotateVectorInverse(local_basis, r);

          if (!is_miss) {
            is_miss = CastVisibilityRay(hit_info.GetXOffset(), r, 0.001f, INF,
                                        mip_and_cone, tlas, Moer::RTVM_ALL,
                                        pt_desc.ray_flags);
          }
          // to world space
          if (!is_miss) {
            samples_num++;
          }
          if (!is_miss || sample_idx == 0) {
            ray = r;
          }
        }

        if (samples_num != 0) {
          path_throughput *= float(samples_num) / float(max_sample_num);
        }

        float a = dot(hit_info.n, ray);
        if (a < 0.f) {
          if (is_diffuse) {
            path_throughput = 0.f;
          } else {
            // specular reflection
            float b = dot(hit_info.n, mat.n);
            ray = normalize(ray +
                            mat.n * STL::Math::Sqrt01(1.f - a * a) * rcp(b));
          }
        }

        // update path throughput
        {
          float3 albedo, Rf0;
          STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(
              mat.base_color, mat.metalness, albedo, Rf0);
          float3 h = normalize(ray + hit_info.v);
          float voh = abs(dot(hit_info.v, h));
          float nol = saturate(dot(hit_info.n, ray));

          if (is_diffuse) {
            float nov = abs(dot(mat.n, hit_info.v));
            path_throughput *=
                STL::Math::Pi(1.f) * albedo *
                STL::BRDF::DiffuseTerm_Burley(mat.roughness, nol, nov, voh);

          } else {
            float3 f = STL::BRDF::FresnelTerm_Schlick(Rf0, voh);
            path_throughput *=
                f * STL::BRDF::GeometryTerm_Smith(mat.roughness, nol);
          }
        }

        {
          // not using russian roulette
          if (STL::Color::Luminance(path_throughput) < 0.01f) {
            break;
          }
        }

        hit_info =
            CastRay(hit_info.GetXOffset(), ray, 0.001f, INF, mip_and_cone, tlas,
                    Moer::RTVM_ALL, pt_desc.ray_flags);
        mat = GetMaterialProps(hit_info);
      }

      // current point lighting
      {
        float4 l_cached = float4(0.f, 0.f, 0.f, 0.f); // came from previous
                                                      // frame

        if (l_cached.w != 1.f) {
          float3 l = mat.l_direct;
          if (STL::Color::Luminance(l) > 0.f) {

            float2 rnd = STL::Rng::Hash::GetFloat2();
            rnd = STL::ImportanceSampling::Cosine::GetRay(rnd).xy;
            rnd *= rt_config.tan_sun_angular_radius;

            float3x3 sun_basis =
                STL::Geometry::GetBasis(rt_config.sun_direction_gexposure.xyz);

            float3 sun_direction = normalize(
                sun_basis[0] * rnd.x + sun_basis[1] * rnd.y + sun_basis[2]);

            float2 mip_and_cone = Raytracing::GetConeAngleFromRoughness(
                hit_info.mip, mat.roughness);

            l *= CastVisibilityRay(hit_info.GetXOffset(), sun_direction, 0.f,
                                   INF, mip_and_cone, tlas, Moer::RTVM_ALL,
                                   pt_desc.ray_flags);
          }
          l += mat.l_emi.xyz;
          l_cached.xyz = lerp(l, l_cached.xyz, l_cached.w);
        }

        // accumulate lighting
        float3 l = l_cached.xyz * path_throughput;
        l_sum += l;

        // biased
        path_throughput *= 1.f - l_cached.w;

        // accumulate distance for NRD

        float a = STL::Color::Luminance(l);
        float b = STL::Color::Luminance(l_sum);

        float importance = a / (b + 0.001f);
        importance *= 1.f - STL::Color::Luminance(mat.l_emi) * rcp(a + 1e-6);

        float diffuse_like_motion =
            Raytracing::EstimateDiffuseProbability(hit_info, mat, true);
        diffuse_like_motion =
            lerp(diffuse_like_motion, 1.f, STL::Math::Sqrt01(mat.curvature));
        diffuse_like_motion = is_diffuse ? 1.f : diffuse_like_motion;

        accumulate_hit_dist +=
            hit_info.tmin *
            STL::Math::SmoothStep(0.2f, 0.f, accumulate_diffuse_like_motion);
        accumulate_diffuse_like_motion +=
            1.f - importance * (1.f - diffuse_like_motion);
      }
    }

    path_throughput *= Raytracing::GetAmbientBRDF(hit_info, mat);
    path_throughput *=
        1.f + Raytracing::EstimateDiffuseProbability(hit_info, mat, true);

    // TODO: add ambient

    float norm_hit_distance = accumulate_hit_dist;
    if (is_diffuse_path) {
      pt_result.diffuse_radiance += l_sum;
      pt_result.diffuse_hit_dist += norm_hit_distance;
      diffuse_path_num++;
    } else {
      pt_result.specular_radiance += l_sum;
      pt_result.specular_hit_dist += norm_hit_distance;
      //   printf("specular path flux: %f", STL::Color::Luminance(l_sum));
    }
  }

  float3 albedo, Rf0;

  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(
      pt_desc.mat.base_color, pt_desc.mat.metalness, albedo, Rf0);

  float nov = abs(dot(pt_desc.mat.n, pt_desc.hit_info.v));
  float3 fenv = STL::BRDF::EnvironmentTerm_Rtg(Rf0, nov, pt_desc.mat.roughness);
  float3 diff_demod = (1.f - fenv) * albedo * 0.99 + 0.01;
  float3 spec_demod = fenv * 0.99 + 0.01;

  float radiance_norm = 1.f / float(pt_desc.path_num);
  pt_result.diffuse_radiance *= radiance_norm;
  pt_result.specular_radiance *= radiance_norm;

  float diff_norm = diffuse_path_num == 0 ? 0.f : 1.f / float(diffuse_path_num);
  pt_result.diffuse_hit_dist *= diff_norm;

  float spec_norm = pt_desc.path_num == diffuse_path_num
                        ? 0.f
                        : 1.f / float(path_num - diffuse_path_num);
  pt_result.specular_hit_dist *= spec_norm;

  return pt_result;
}
// dispatch inline raytracing

[numthreads(16, 16, 1)] void main(uint2 pixel_pos
                                  : SV_DispatchThreadID) {
  if (any(pixel_pos >= param.rect)) {
    return;
  }

  float2 uv = (float2(pixel_pos) + 0.5f) * param.inv_rect;

  ArrayBuffer instance_buffer = ArrayBuffer(param.instance_buffer_handle);

  ArrayBuffer global_params = ArrayBuffer(param.global_param_handle);

  // uint array_handle =
  //     g__array_114514_bdls[NonUniformResourceIndex(global_params.handle)];
  //     printf("array_handle %d\n", array_handle);

  RTViewParam view = global_params.Load<RTViewParam>(0);

  float3 cam_ray_origion_v = Raytracing::ReconstructViewPosition(
      uv, view.frustum, -view.near_far.x, view.orthomode);
  float3 cam_ray_origion_w =
      mul(float4(cam_ray_origion_v, 1.0f), view.view2world).xyz;

  float3 cam_ray_dir_w =
      view.orthomode == 0
          ? normalize(mul(cam_ray_origion_v, (float3x3)view.view2world))
          : -view.dir;
  // if(pixel_pos.x == 140 && pixel_pos.y == 140)
  //   printf("cam_ray_dir_w %f %f %f\n", cam_ray_dir_w.x, cam_ray_dir_w.y,
  //          cam_ray_dir_w.z);
  // RTHitInfo hit_info = (RTHitInfo)0;
  // RTMaterialProp mat = (RTMaterialProp)0;
  // float2 mip_and_cone = Raytracing::GetConeAngleFromRoughness(0.f, 0.f);
  // printf("mip and cone %f %f\n", mip_and_cone.x, mip_and_cone.y);
  RTHitInfo hit_info = CastRay(cam_ray_origion_w, cam_ray_dir_w, 0.001f, INF,
                               Raytracing::GetConeAngleFromRoughness(0.f, 0.f),
                               tlas, Moer::RTVM_ALL, 0);

  RTMaterialProp mat = GetMaterialProps(hit_info);

  // view z
  float view_z = mul(float4(hit_info.x, 1.f), view.world2view).z;
  view_z = hit_info.IsSky() ? STL::Math::Sign(view_z) * INF : view_z;
  out_view_z[pixel_pos] = view_z;

  // motion
  float3 motion = Raytracing::GetMotion(hit_info.x, hit_info.x_prev);
  out_mv[pixel_pos] = motion;

  if (hit_info.IsSky()) {
    out_shadow_info[pixel_pos] = float2(0.f, 0.f);
    out_emission[pixel_pos] = mat.l_emi;
    out_direct_lighting[pixel_pos] = mat.l_emi;
    return;
  }

  float diffuse_probablility =
      Raytracing::EstimateDiffuseProbability(hit_info, mat);

  out_normal_roughness[pixel_pos] = float4(hit_info.n, mat.roughness);
  out_basecolor_metalness[pixel_pos] =
      float4(STL::Color::LinearToSrgb(mat.base_color), mat.metalness);

  out_direct_lighting[pixel_pos] = mat.l_direct;
  out_emission[pixel_pos] = mat.l_emi;

  float3 shadow_translucency = 0.f;

  float shadow_distance = 0.f;
  {
    // show ray
    float2 rnd;
    STL::Rng::Hash::Initialize(pixel_pos, param.frame_index);
    rnd = STL::Rng::Hash::GetFloat2();

    rnd = STL::ImportanceSampling::Cosine::GetRay(rnd).xy;
    rnd *= rt_config.tan_sun_angular_radius; // angular radius
    float3x3 m_sun_basis =
        STL::Geometry::GetBasis(rt_config.sun_direction_gexposure.xyz);
    float3 sun_direction = normalize(m_sun_basis[0] * rnd.x +
                                     m_sun_basis[1] * rnd.y + m_sun_basis[2]);
    float3 x_offset = hit_info.GetXOffset();
    float2 mip_and_cone = Raytracing::GetConeAngleFromRoughness(
        hit_info.mip, rt_config.tan_sun_angular_radius);

    shadow_translucency =
        (STL::Color::Luminance(mat.l_direct) != 0.f) ? 1.0f : 0.0f;

    while (STL::Color::Luminance(shadow_translucency) > 0.01f) {
      RTHitInfo shadow_hit_info =
          CastRay(x_offset, sun_direction, 0.001f, INF, mip_and_cone, tlas,
                  Moer::RTVM_ALL, 0);
      if (shadow_hit_info.IsSky()) {
        shadow_distance = shadow_distance == 0.f ? INF : shadow_distance;
        break;
      }

      // glass approximation
      float NoV = abs(dot(shadow_hit_info.n, sun_direction));
      shadow_translucency *= lerp(shadow_hit_info.IsTransparent() ? 0.9f : 0.f,
                                  0.f, STL::Math::Pow01(1.0 - NoV, 2.5f));

      float offset = shadow_hit_info.tmin * 0.0001f + 0.001f;
      x_offset = shadow_hit_info.GetXOffset() + sun_direction * offset;
      shadow_distance += shadow_hit_info.tmin;
    }

    float2 shadow_info =
        float2(shadow_distance == INF ? INF : shadow_distance, 0.f);
    out_shadow_info[pixel_pos] = shadow_info;
  }

  // secondary ray
  PathTracingDesc pt_desc = (PathTracingDesc)0;
  pt_desc.hit_info = hit_info;
  pt_desc.mat = mat;
  pt_desc.pixel_pos = pixel_pos;
  pt_desc.path_num = 1;
  pt_desc.bounce_num = rt_config.bounce_num;
  pt_desc.instance_mask = Moer::RTVM_ALL;
  pt_desc.ray_flags = 0;

  PathTracingResult pt_result = PathTracing(pt_desc);

  float3 l_sum = mat.l_direct * (shadow_translucency) + mat.l_emi;

  // composition

  float3 albedo, Rf0;
  STL::BRDF::ConvertBaseColorMetalnessToAlbedoRf0(mat.base_color, mat.metalness,
                                                  albedo, Rf0);
  float nov = abs(dot(hit_info.n, hit_info.v));
  float3 f_env = STL::BRDF::EnvironmentTerm_Rtg(Rf0, nov, mat.roughness);

  float3 diff_demod = (1.0 - f_env) * albedo * 0.99 + 0.01;
  float3 spec_demod = f_env * 0.99 + 0.01;

  float3 l_diff = pt_result.diffuse_radiance * diff_demod;
  float3 l_spec = pt_result.specular_radiance * spec_demod;

  //   if (STL::Color::Luminance(shadow_translucency) > 0.0f)
  //     printf("mat.l_direct %f %f %f shadow_distance %f\n", mat.l_emi,
  //     mat.l_emi,
  //            mat.l_emi, shadow_translucency);
  //   out_position[pixel_pos] = float4(hit_info.x, 1.0f);
  out_direct_lighting[pixel_pos] = l_sum + l_diff + l_spec;
  out_diffuse[pixel_pos] =
      float4(pt_result.diffuse_radiance, pt_result.diffuse_hit_dist);
  out_specular[pixel_pos] =
      float4(pt_result.specular_radiance, pt_result.specular_hit_dist);
}