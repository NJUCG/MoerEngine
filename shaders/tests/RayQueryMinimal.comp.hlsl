[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    RayDesc ray_desc;
    ray_desc.Origin = float3(dispatch_thread_id.xy, 0.0);
    ray_desc.Direction = float3(0.0, 0.0, 1.0);
    ray_desc.TMin = 0.0;
    ray_desc.TMax = 1.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;
    ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, ray_desc);
    ray_query.Proceed();
}