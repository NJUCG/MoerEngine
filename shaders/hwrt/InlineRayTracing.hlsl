#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
RTCONFIG_BINDING(1, 0);
BINDLESS_BINDINGS(3, 2, 4, 5);
#include <framework/Lighting.hlsl>

#include <framework/RaytracingShared.hlsli>

#include <framework/Material.hlsl>

struct MaterialData {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float ao;
  uint albedo_map;
  int normal_map;
  int metallic_roughness_map;
  int ao_map;
  int emissive_map;
  int padding;
};

struct Param {
  uint instance_buffer_handle;
  uint material_buffer_handle;
  uint primitive_buffer_handle;
  uint vtx_buffer_handle;
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
[[vk::binding(0, 1)]] RWTexture2D<float4> out_normal : register(u0, space1);
[[vk::binding(1, 1)]] RWTexture2D<float4> out_color : register(u1, space1);
[[vk::binding(2, 1)]] RWTexture2D<float4> out_position : register(u2, space1);
[[vk::binding(3, 1)]] RWTexture2D<float4> out_direct_lighting
    : register(u3, space1);

#define SKY_INTENSITY 1.0
#define SUN_INTENSITY 8.0

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
  ArrayBuffer primitive_buffer = ArrayBuffer(param.primitive_buffer_handle);
  ArrayBuffer vtx_buffer = ArrayBuffer(param.vtx_buffer_handle);

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

    float3x3 model2world = (float3x3)ray_query.CommittedObjectToWorld3x4();

    uint instance_id =
        ray_query.CandidateInstanceID() + ray_query.CandidateGeometryIndex();
    hit_info.instance_id = instance_id;

    RTInstanceData instance_data =
        instance_buffer.Load<RTInstanceData>(instance_id);

    uint primitive_id =
        instance_data.prim_offset + ray_query.CandidatePrimitiveIndex();

    RTPrimitive primitive = primitive_buffer.Load<RTPrimitive>(primitive_id);
    RTVertex vtx[3];
    vtx[0] = vtx_buffer.Load<RTVertex>(primitive.indices.x +
                                       instance_data.vtx_offset);
    vtx[1] = vtx_buffer.Load<RTVertex>(primitive.indices.y +
                                       instance_data.vtx_offset);
    vtx[2] = vtx_buffer.Load<RTVertex>(primitive.indices.z +
                                       instance_data.vtx_offset);
    float3 barycentrics;
    barycentrics.yz = ray_query.CandidateTriangleBarycentrics();
    barycentrics.x = 1.0f - barycentrics.y - barycentrics.z;

    float2 uv = barycentrics.x * float2(vtx[0].uv0, vtx[0].uv1) +
                barycentrics.y * float2(vtx[1].uv0, vtx[1].uv1) +
                barycentrics.z * float2(vtx[2].uv0, vtx[2].uv1);
    float3 n = barycentrics.x * vtx[0].normal + barycentrics.y * vtx[1].normal +
               barycentrics.z * vtx[2].normal;
    n = mul(model2world, n);
    n = normalize(n);

    hit_info.n = n;
    hit_info.uv = uv;

    float3 t = barycentrics.x * vtx[0].tangent +
               barycentrics.y * vtx[1].tangent +
               barycentrics.z * vtx[2].tangent;
    t = mul(model2world, t);
    t = normalize(t);

    hit_info.t = t;

    float NoR = abs(dot(hit_info.n, direction));

    float a = hit_info.tmin * mip_and_cone.y;
    a *= 1.f / NoR;
    a *= primitive.world_uv_units;
    // printf("a %f\n", a);

    float mip = log2(a);
    mip += MAX_MIP - 2;
    mip = max(0.f, mip);
    hit_info.mip += mip;

    hit_info.x = origin + direction * hit_info.tmin;
    hit_info.x_prev = hit_info.x;
    hit_info.flags = instance_data.flags;
    hit_info.material_type_and_id = instance_data.material_type_and_id;
  }
  hit_info.v = -direction;
  return hit_info;
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
  TextureHandle albedo_map = TextureHandle(mat_data.albedo_map);

  float3 coords = Raytracing::GetSamplingCoords(albedo_map.handle, hit_info.uv,
                                                hit_info.mip, MIP_SHARP);
  //   printf("mip %f\n", coords.z);
  //   float3 coords = Raytracing::GetLoadCoords(albedo_map.handle, hit_info.uv,
  //   hit_info.mip, MIP_SHARP); printf("coords %f %f %f\n", coords.x, coords.y,
  //   coords.z);
  float4 color = albedo_map.SampleLevel2D<float4>(coords.xy, 0.f);
  //    color.xyz = STL::Color::GammaToLinear(color.xyz);
  float3 base_color = color.xyz;
  // Texture2D<float4> albedo = albedo_map.GetTexture2D<float4>();
  // SamplerHandle sp = (SamplerHandle)0;
  // mat.base_color = albedo.SampleLevel(sp.GetSampler(), coords.xy,
  // coords.z).xyz;
  //   mat.base_color = albedo.Load(int3(coords)).xyz;
  // normal
  float3 n = hit_info.n;
  //   if (mat_data.normal_map != -1) {
  //     TextureHandle normal_map = TextureHandle(mat_data.normal_map);
  //     coords = Raytracing::GetSamplingCoords(normal_map.handle, hit_info.uv,
  //                                            hit_info.mip, MIP_LESS_SHARP);
  //     n = normal_map.SampleLevel2D<float4>(coords.xy, coords.z).xyz;
  //     n = normalize(n * 2.0f - 1.0f);
  //   }
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
  float3 l_emi = float3(0.0, 0.0, 0.0);

  float emission_level = STL::Color::Luminance(mat.l_emi);
  emission_level = saturate(emission_level * 50.0);

  metalness = lerp(metalness, 0.0, emission_level);
  roughness = lerp(roughness, 1.0, emission_level);

  // printf("metalness %f roughness %f\n", metalness, roughness);

  ArrayBuffer light_buffer = ArrayBuffer(param.light_buffer_handle);
  LightData sun_light = light_buffer.Load<LightData>(0);

  float3 l_direct = (float3)0;

  float NoL = saturate(dot(n, sun_direction_exposure.xyz));
  float shadow = STL::Math::SmoothStep(0.03, 0.1, NoL);
  // if (n.y > 0.f)
  //   printf("n %f %f %f %f\n", n.x, n.y, n.z, NoL);
  [branch] if (shadow != 0.f) {
    float3 c_sun =
        GetSunIntensity(sun_direction_exposure.xyz, sun_direction_exposure.xyz,
                        rt_config.tan_sun_angular_radius);

    // printf("c_sun %f %f %f\n", c_sun.x, c_sun.y, c_sun.z);

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

// dispatch inline raytracing

[numthreads(16, 16, 1)] void main(uint2 pixel_pos
                                  : SV_DispatchThreadID) {
  if (any(pixel_pos >= param.rect)) {
    return;
  }

  float2 uv = (float2(pixel_pos) + 0.5f) * param.inv_rect;

  ArrayBuffer instance_buffer = ArrayBuffer(param.instance_buffer_handle);
  ArrayBuffer global_params = ArrayBuffer(param.global_param_handle);

  RTViewParam view = global_params.Load<RTViewParam>(0);

  float3 cam_ray_origion_v = Raytracing::ReconstructViewPosition(
      uv, view.frustum, -view.near_far.x, view.orthomode);
  float3 cam_ray_origion_w =
      mul(float4(cam_ray_origion_v, 1.0f), view.view2world).xyz;
  float3 cam_ray_dir_w =
      view.orthomode == 0
          ? normalize(mul((float3x3)view.view2world, cam_ray_origion_v))
          : -view.dir;
  // RTHitInfo hit_info = (RTHitInfo)0;
  // RTMaterialProp mat = (RTMaterialProp)0;
  // float2 mip_and_cone = Raytracing::GetConeAngleFromRoughness(0.f, 0.f);
  // printf("mip and cone %f %f\n", mip_and_cone.x, mip_and_cone.y);
  RTHitInfo hit_info = CastRay(cam_ray_origion_w, cam_ray_dir_w, 0.001f, INF,
                               Raytracing::GetConeAngleFromRoughness(0.f, 0.f),
                               tlas, INSTANCE_FLAG_GEOMETRY_ALL, 0);

  RTMaterialProp mat = GetMaterialProps(hit_info);

  float3 shadow_translucency = 0.f;

  float shadow_distance = 0.f;
  {
    // show ray
    float2 rnd;
    STL::Rng::Hash::Initialize(pixel_pos, param.frame_index);
    rnd = STL::Rng::Hash::GetFloat2();

    rnd = STL::ImportanceSampling::Cosine::GetRay(rnd).xy;
    rnd *= 0.1f; // angular radius
    float3x3 m_sun_basis =
        STL::Geometry::GetBasis(rt_config.sun_direction_gexposure.xyz);
    float3 sun_direction = normalize(m_sun_basis[0] * rnd.x +
                                     m_sun_basis[1] * rnd.y + m_sun_basis[2]);
    float3 x_offset = hit_info.GetXOffset();
    float2 mip_and_cone = Raytracing::GetConeAngleFromRoughness(
        hit_info.mip, rt_config.tan_sun_angular_radius);
    shadow_translucency = 1.f;
    while (STL::Color::Luminance(shadow_translucency) > 0.01f) {
      RTHitInfo shadow_hit_info =
          CastRay(x_offset, sun_direction, 0.001f, INF, mip_and_cone, tlas,
                  INSTANCE_FLAG_GEOMETRY_ALL, 0);
      if (shadow_hit_info.IsSky()) {
        // shadow_translucency =
        //     shadow_distance == 0.f ? 0.f : shadow_translucency;
        shadow_distance = shadow_distance == 0.f ? INF : shadow_distance;
        // printf("sky shadow translucency %f %f %f\n", shadow_translucency,
        //        shadow_hit_info.tmin, shadow_distance);
        break;
      }

      // glass approximation
      float NoV = abs(dot(shadow_hit_info.n, sun_direction));
      shadow_translucency *= lerp(0.9f, 0.f, STL::Math::Pow01(1.0 - NoV, 2.5f));
      shadow_translucency *= float(shadow_hit_info.IsTransparent());

      float offset = shadow_hit_info.tmin * 0.0001f + 0.001f;
      x_offset = shadow_hit_info.GetXOffset() + sun_direction * offset;
      shadow_distance += shadow_hit_info.tmin;
      // printf("occluded shadow translucency %f %f %f\n", shadow_translucency,
      //        shadow_hit_info.tmin, shadow_distance);
    }
  }
  float3 l_sum = mat.l_direct * (shadow_translucency) + mat.l_emi;
  // if (STL::Color::Luminance(mat.l_direct) > 0.0f)
  //   printf("mat.l_direct %f %f %f shadow_distance %f\n", mat.l_emi,
  //   mat.l_emi,
  //          mat.l_emi, shadow_translucency);

  out_normal[pixel_pos] = float4(hit_info.n, 1.0f);
  out_color[pixel_pos] = float4(mat.base_color, 1.0f);
  out_position[pixel_pos] = float4(hit_info.x, 1.0f);
  out_direct_lighting[pixel_pos] = float4(l_sum, 1.0f);
}