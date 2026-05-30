[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output_buffer : register(u0);

#include <core/common/Bindless.hlsl>
#include <core/common/Common.hlsl>

BINDLESS_BINDINGS(1)

#include <materials/Material.hlsli>
#include <shared/Geometry.h>
#include <shared/ShaderParameters.h>
#include <shared/utils/MoerMath.hlsli>
#include <shared/utils/Packing.h>
#include <core/math/STL.hlsli>
#include <pipelines/raytracing/inline/RaytracingCommon.hlsli>

[[vk::push_constant]] ConstantBuffer<Moer::GBufferPassParams> param;

static const uint kViewportWidth = 10;
static const uint kViewportHeight = 10;
static const uint kWordsPerPixel = 16;
static const uint kHitBase = 0x5cee0000u;

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  if (dispatch_thread_id.x >= kViewportWidth || dispatch_thread_id.y >= kViewportHeight) {
    return;
  }

  float2 uv = (float2(dispatch_thread_id.xy) + 0.5.xx) / float2(kViewportWidth, kViewportHeight);
  float2 ray_xy = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

  RayDesc ray_desc;
  ray_desc.Origin = float3(ray_xy, -1.0);
  ray_desc.Direction = float3(0.0, 0.0, 1.0);
  ray_desc.TMin = 0.0;
  ray_desc.TMax = 4.0;

  RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;
  ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray_desc);
  while (ray_query.Proceed()) {}

  if (ray_query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    return;
  }

  uint instance_id = ray_query.CommittedInstanceID();
  uint primitive_id = ray_query.CommittedPrimitiveIndex();
  float2 barycentrics = ray_query.CommittedTriangleBarycentrics();
  Moer::GeometryRecord geometry = Moer::GetGeometryRecordFrom(
    param,
    instance_id,
    primitive_id,
    barycentrics,
    Moer::EGA_Position | Moer::EGA_UV,
    !ray_query.CommittedTriangleFrontFace()
  );

  uint base = (dispatch_thread_id.y * kViewportWidth + dispatch_thread_id.x) * kWordsPerPixel;
  output_buffer[base + 0] = kHitBase | ((dispatch_thread_id.y & 0xffu) << 8) | (dispatch_thread_id.x & 0xffu);
  output_buffer[base + 1] = instance_id;
  output_buffer[base + 2] = primitive_id;
  output_buffer[base + 3] = geometry.instance.primitive_id;
  output_buffer[base + 4] = geometry.primitive.position_start_idx;
  output_buffer[base + 5] = geometry.primitive.index_start_idx;
  output_buffer[base + 6] = asuint(geometry.vtx_positions[0].x);
  output_buffer[base + 7] = asuint(geometry.vtx_positions[0].y);
  output_buffer[base + 8] = asuint(geometry.vtx_positions[1].x);
  output_buffer[base + 9] = asuint(geometry.vtx_positions[1].y);
  output_buffer[base + 10] = asuint(geometry.material.roughness_factor);
  output_buffer[base + 11] = asuint(geometry.vtx_uvs[0].x);
  output_buffer[base + 12] = asuint(ray_query.CommittedRayT());
  output_buffer[base + 13] = asuint(geometry.model_pos.z);
  output_buffer[base + 14] = ray_query.CommittedGeometryIndex();
  output_buffer[base + 15] = 0x600d0001u;
}

 // Ensure there is a newline at the end of the file
