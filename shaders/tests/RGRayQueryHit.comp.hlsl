[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output_buffer : register(u0);

static const uint kViewportWidth = 10;
static const uint kViewportHeight = 10;
static const uint kHitBase = 0x7a510000u;

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

  uint output_index = dispatch_thread_id.y * kViewportWidth + dispatch_thread_id.x;
  uint debug_value = kHitBase | ((dispatch_thread_id.y & 0xffu) << 8) | (dispatch_thread_id.x & 0xffu);
  output_buffer[output_index] = ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? debug_value : 0u;
}
