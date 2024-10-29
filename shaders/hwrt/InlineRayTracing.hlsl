#include <framework/Bindless.hlsl>
#include <framework/Common.hlsl>
#include <framework/RaytracingShared.hlsli>

BINDLESS_BINDINGS(3, 2, 4, 5)

struct Param{
  uint instance_buffer_handle;
  uint mesh_info_buffer_handle;
  uint primitive_buffer_handle;
  uint vtx_buffer_handle;
  uint global_param_handle;
  uint2 rect;
  float2 inv_rect;
  float2 jitter;
};

[[vk::push_constant]] ConstantBuffer<Param> param;
[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas : register(t0, space0);
[[vk::binding(0, 1)]] RWTexture2D<float4> out_normal : register(u0, space1);
[[vk::binding(1, 1)]] RWTexture2D<float4> out_color : register(u1, space1);
[[vk::binding(2, 1)]] RWTexture2D<float4> out_position : register(u2, space1);


RTHitInfo CastRay(float3 origin, float3 direction, float tmin, float tmax, RaytracingAccelerationStructure accel, uint instance_mask, uint ray_flags){
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


    while(ray_query.Proceed())
    {        
        // uint instance_id = ray_query.CandidateInstanceID() + ray_query.CandidateGeometryIndex();

        // RTInstanceData instance_data = instance_buffer.Load<RTInstanceData>(instance_id);

        // uint primitive_id = instance_data.prim_offset + ray_query.CandidatePrimitiveIndex();

        // RTPrimitive primitive = primitive_buffer.Load<RTPrimitive>(primitive_id);
        // RTVertex vtx[3];
        // vtx[0] = vtx_buffer.Load<RTVertex>(primitive.indices.x + instance_data.vtx_offset);
        // vtx[1] = vtx_buffer.Load<RTVertex>(primitive.indices.y + instance_data.vtx_offset);
        // vtx[2] = vtx_buffer.Load<RTVertex>(primitive.indices.z + instance_data.vtx_offset);

        // float3 barycentrics;
        // barycentrics.yz = ray_query.CandidateTriangleBarycentrics();
        // barycentrics.x = 1.0f - barycentrics.y - barycentrics.z;

        // float2 uv = barycentrics.x * float2(vtx[0].uv0, vtx[0].uv1) + barycentrics.y * float2(vtx[1].uv0, vtx[1].uv1) + barycentrics.z * float2(vtx[2].uv0, vtx[2].uv1);

        ray_query.CommitNonOpaqueTriangleHit();
    }
    RTHitInfo hit_info = (RTHitInfo)0;

    if(ray_query.CommittedStatus() == COMMITTED_NOTHING){
        hit_info.tmin = ray.TMax;
        hit_info.x = origin + direction * hit_info.tmin;
        hit_info.x_prev = hit_info.x;

    }else{
        hit_info.tmin = ray_query.CommittedRayT();
        
        float3x3 model2world = (float3x3)ray_query.CommittedObjectToWorld3x4();

        uint instance_id = ray_query.CandidateInstanceID() + ray_query.CandidateGeometryIndex();
        hit_info.instance_id = instance_id;

        RTInstanceData instance_data = instance_buffer.Load<RTInstanceData>(instance_id);

        uint primitive_id = instance_data.prim_offset + ray_query.CandidatePrimitiveIndex();

        RTPrimitive primitive = primitive_buffer.Load<RTPrimitive>(primitive_id);
        RTVertex vtx[3];
        vtx[0] = vtx_buffer.Load<RTVertex>(primitive.indices.x + instance_data.vtx_offset);
        vtx[1] = vtx_buffer.Load<RTVertex>(primitive.indices.y + instance_data.vtx_offset);
        vtx[2] = vtx_buffer.Load<RTVertex>(primitive.indices.z + instance_data.vtx_offset);

        float3 barycentrics;
        barycentrics.yz = ray_query.CandidateTriangleBarycentrics();
        barycentrics.x = 1.0f - barycentrics.y - barycentrics.z;

        float2 uv = barycentrics.x * float2(vtx[0].uv0, vtx[0].uv1) + barycentrics.y * float2(vtx[1].uv0, vtx[1].uv1) + barycentrics.z * float2(vtx[2].uv0, vtx[2].uv1);
        float3 n = barycentrics.x * vtx[0].normal + barycentrics.y * vtx[1].normal + barycentrics.z * vtx[2].normal;
        n = mul(model2world, n);
        n = normalize(n);

        hit_info.n = n;
        hit_info.uv = uv;


        float3 t = barycentrics.x * vtx[0].tangent + barycentrics.y * vtx[1].tangent + barycentrics.z * vtx[2].tangent;
        t = mul(model2world, t);
        t = normalize(t);

        hit_info.t = t;
        hit_info.x = origin + direction * hit_info.tmin;
        hit_info.x_prev = hit_info.x;
    }
    hit_info.v = -direction;
    return hit_info;
}

//dispatch inline raytracing

[numthreads( 16, 16, 1 )]
void main( uint2 pixel_pos : SV_DispatchThreadID )
{
   if(any(pixel_pos >= param.rect))
   {
     return;
   }

   float2 uv = (float2(pixel_pos) + 0.5f) * param.inv_rect;

   ArrayBuffer instance_buffer = ArrayBuffer(param.instance_buffer_handle);
   ArrayBuffer global_params = ArrayBuffer(param.global_param_handle);

   RTViewParam view = global_params.Load<RTViewParam>(0);

   float3 cam_ray_origion_v = Raytracing::ReconstructViewPosition(uv, view.frustum, -view.near_far.x, view.orthomode);
   float3 cam_ray_origion_w = mul(float4(cam_ray_origion_v, 1.0f), view.view2world).xyz;
   float3 cam_ray_dir_w = view.orthomode == 0 ? normalize(mul((float3x3)view.view2world, cam_ray_origion_v)) : -view.dir;

    RTHitInfo hit_info = CastRay(cam_ray_origion_w, cam_ray_dir_w, 0.001f, INF, tlas, INSTANCE_FLAG_GEOMETRY_ALL, 0);

    out_normal[pixel_pos] = float4(hit_info.n, 1.0f);
    out_color[pixel_pos] = float4(1.0f, 0.0f, 0.0f, 1.0f);
    out_position[pixel_pos] = float4(hit_info.x, 1.0f);

}