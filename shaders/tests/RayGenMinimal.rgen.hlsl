[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;

struct [raypayload] Payload {
    float3 hit_value;
};

[shader("raygeneration")]
void main() {
    RayDesc ray_desc;
    ray_desc.Origin = float3(0.0, 0.0, 0.0);
    ray_desc.Direction = float3(0.0, 0.0, 1.0);
    ray_desc.TMin = 0.0;
    ray_desc.TMax = 1.0;

    Payload payload = (Payload)0;
    TraceRay(tlas, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray_desc, payload);
}