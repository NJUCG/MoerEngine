#ifndef MOER_GBUFFER_RT_HLSL
#define MOER_GBUFFER_RT_HLSL

// bind bindless
#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>

BINDLESS_BINDINGS(3, 2, 4, 5);

#include <materials/Material.hlsl>

#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>

#include <core/math/STL.hlsli>
#include <hwrt/GBufferUtils.hlsli>

#include <pipelines/raytracing/inline/RaytracingCommon.hlsli>

[[vk::push_constant]] ConstantBuffer<Moer::GBufferPassParams> param;

[[vk::binding(0, 0)]] ConstantBuffer<Moer::GBufferConstants> gbuffer_constants;

[[vk::binding(1, 0)]] RWTexture2D<float> gbuffer_view_depth;
[[vk::binding(2, 0)]] RWTexture2D<uint> gbuffer_diffuse_albedo;

[[vk::binding(3, 0)]] RWTexture2D<uint> gbuffer_specular_roughness;
[[vk::binding(4, 0)]] RWTexture2D<uint> gbuffer_normal;

[[vk::binding(5, 0)]] RWTexture2D<float4> gbuffer_emissive;
[[vk::binding(6, 0)]] RWTexture2D<float4> gbuffer_motion;

[[vk::binding(7, 0)]] RWTexture2D<float> gbuffer_clip_depth;

[[vk::binding(8, 0)]] RaytracingAccelerationStructure tlas;

struct RayPayload {
  float min_glass_ray_t;
  float committed_ray_t;
  uint instance_id;
  uint geom_id;
  uint prim_id;
  float2 barycentrics;
  bool b_backface;
};

bool ProcessAnyHit(inout RayPayload payload, uint instance_id, uint geom_id,
                   uint prim_id, float2 barycentrics, float t,
                   bool b_backface = false) {

  ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data_handle);
  ArrayBuffer geom_data_array = ArrayBuffer(param.geometry_data_handle);
  ArrayBuffer material_data_array = ArrayBuffer(param.material_data_handle);
  Moer::GeometryRecord geom = Moer::GetGeometryRecordFrom(
      instance_id, geom_id, prim_id, barycentrics, Moer::EGA_UV,
      instance_data_array.GetByteAddressBuffer(),
      geom_data_array.GetByteAddressBuffer(),
      material_data_array.GetByteAddressBuffer());

  Moer::MaterialSample mat_sample = Moer::SampleGeometryMaterial(
      geom, 0.f, 0.f, 0.f, Moer::EMA_BaseColor | Moer::EMA_Transmission);

  bool alpha_mask = mat_sample.opacity > 0.5f; // TODO: alpha cut off
  // todo: update glass info here
  return alpha_mask;
}

// test
float3 UintHashToColor(uint _idx) {
  uint hash = STL::Sequence::Hash(_idx);
  // r11g11b10

  float r = (hash & 0x7ff) / 2047.f;
  float g = ((hash >> 11) & 0x7ff) / 2047.f;
  float b = ((hash >> 22) & 0x3ff) / 1023.f;

  return float3(r, g, b);
}

#define USE_RAYQUERY 1
#if USE_RAYQUERY
[numthreads(16, 16, 1)] void main(uint2 pixel_pos
                                  : SV_DispatchThreadID)
#else
[shader("raygeneration")] void RayGen()
#endif
{
#if !USE_RAYQUERY
  uint2 pixel_pos = DispatchRaysIndex().xy;
#endif
  RayDesc ray = Moer::SetupPrimaryRay(pixel_pos, gbuffer_constants.main_view);

  uint instance_mask = Moer::RTVM_ALL;
  uint ray_flags = RAY_FLAG_NONE;

  //   if(pixel_pos.x == 140 && pixel_pos.y == 140)
  //   printf("ray direction %f %f %f\n", ray.Direction.x, ray.Direction.y,
  //          ray.Direction.z);
  RayPayload payload;
  payload.min_glass_ray_t = ray.TMax + 1.f;
  payload.committed_ray_t = 0.f;
  payload.instance_id = ~0u;
  payload.prim_id = 0;
  payload.barycentrics = 0.f;
  payload.b_backface = false;

#if USE_RAYQUERY
  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;
  ray_query.TraceRayInline(tlas, ray_flags, instance_mask, ray);
  while (ray_query.Proceed()) {
    if (ray_query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      if (ProcessAnyHit(payload, ray_query.CandidateInstanceID(),
                        ray_query.CandidateGeometryIndex(),
                        ray_query.CandidatePrimitiveIndex(),
                        ray_query.CandidateTriangleBarycentrics(),
                        ray_query.CandidateTriangleRayT())) {
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
    payload.b_backface = !ray_query.CommittedTriangleFrontFace();
    // printf("hit instance %d\n", payload.instance_id);
  }
#else
// TraceRay
#endif

  const float hit_t = payload.committed_ray_t;
  const bool hit_glass = hit_t > payload.min_glass_ray_t;
  const float max_glass_hit_t = hit_glass ? hit_t : 0.f;

  if (payload.instance_id != ~0u) {
    // shader surface

    ArrayBuffer instance_data_array = ArrayBuffer(param.instance_data_handle);
    ArrayBuffer geom_data_array = ArrayBuffer(param.geometry_data_handle);
    ArrayBuffer material_data_array = ArrayBuffer(param.material_data_handle);

    Moer::GeometryRecord geom = Moer::GetGeometryRecordFrom(
        payload.instance_id, payload.geom_id, payload.prim_id,
        payload.barycentrics, Moer::EGA_All,
        instance_data_array.GetByteAddressBuffer(),
        geom_data_array.GetByteAddressBuffer(),
        material_data_array.GetByteAddressBuffer(), payload.b_backface);

    // compute gradient
    RayDesc r0 = ray;
    RayDesc r1 = Moer::SetupPrimaryRay(pixel_pos + uint2(1, 0),
                                       gbuffer_constants.main_view);
    RayDesc r2 = Moer::SetupPrimaryRay(pixel_pos + uint2(0, 1),
                                       gbuffer_constants.main_view);

    float3 pos_ws[3];
    pos_ws[0] =
        mul(geom.instance.model2world, float4(geom.vtx_positions[0], 1.f)).xyz;
    pos_ws[1] =
        mul(geom.instance.model2world, float4(geom.vtx_positions[1], 1.f)).xyz;
    pos_ws[2] =
        mul(geom.instance.model2world, float4(geom.vtx_positions[2], 1.f)).xyz;

    float3 bary0 = Moer::RayIntersectBarycentrics(
        r0.Origin, r0.Direction, pos_ws[0], pos_ws[1], pos_ws[2]);
    float3 baryx = Moer::RayIntersectBarycentrics(
        r1.Origin, r1.Direction, pos_ws[0], pos_ws[1], pos_ws[2]);

    float3 baryy = Moer::RayIntersectBarycentrics(
        r2.Origin, r2.Direction, pos_ws[0], pos_ws[1], pos_ws[2]);

    float2 texcoord_0 = Moer::Interpolate(geom.vtx_uvs, bary0);
    float2 texcoord_x = Moer::Interpolate(geom.vtx_uvs, baryx);
    float2 texcoord_y = Moer::Interpolate(geom.vtx_uvs, baryy);

    float2 tex_grad_x = (texcoord_x - texcoord_0);
    float2 tex_grad_y = (texcoord_y - texcoord_0);
    // sample gradient
    Moer::MaterialSample mat_sample = Moer::SampleGeometryMaterial(
        geom, tex_grad_x, tex_grad_y, -1.f, Moer::EMA_All);

    float clip_depth = 0.f;
    float view_depth = 0.f;
    float3 motion = Moer::GetMotion(
        gbuffer_constants.main_view, gbuffer_constants.prev_view, geom.instance,
        geom.model_pos, geom.model_pos_prev, clip_depth, view_depth);
    float4 clip_xy = mul(
        gbuffer_constants.main_view.world2clip,
        float4(mul(geom.instance.model2world, float4(geom.model_pos, 1.0f)).xyz,
               1.f));

    clip_xy.xyz /= clip_xy.w;

    gbuffer_view_depth[pixel_pos] = view_depth;
    gbuffer_clip_depth[pixel_pos] = clip_depth;
    gbuffer_diffuse_albedo[pixel_pos] =
        Moer::Pack_R11G11B10_UFLOAT(mat_sample.diffuse_albedo);
    gbuffer_specular_roughness[pixel_pos] = Moer::Pack_R8G8B8A8_Gamma_UFLOAT(
        float4(mat_sample.specular_f0, mat_sample.roughness));
    gbuffer_normal[pixel_pos] = Math::NdirToOctUnorm32(mat_sample.normal);

    gbuffer_emissive[pixel_pos] = float4(mat_sample.emissive, max_glass_hit_t);
    gbuffer_motion[pixel_pos] = float4(motion, 0.f);

    // printf("hit depth %d\n", clip_depth);
    return;
  }

  gbuffer_view_depth[pixel_pos] = FP16_MAX;
  gbuffer_clip_depth[pixel_pos] = 0.f;
  gbuffer_diffuse_albedo[pixel_pos] = 0;
  gbuffer_specular_roughness[pixel_pos] = 0;
  gbuffer_normal[pixel_pos] = 0;
  gbuffer_emissive[pixel_pos] = float4(0, 0, 0, max_glass_hit_t);
  gbuffer_motion[pixel_pos] = 0;
}

#endif // MOER_GBUFFER_RT_HLSL