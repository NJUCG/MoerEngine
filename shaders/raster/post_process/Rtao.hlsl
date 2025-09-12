#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/ShaderParameters.h"
#include "shared/raster/post_process/ShaderParameters.h"

#include "framework/Math.hlsli"

#ifndef DI_BINDING_SLOT
#define DI_BINDING_SLOT 0
#endif

[[vk::push_constant]] ConstantBuffer<Moer::RtaoPipelineBindlessParam> param;

[[vk::binding(0, DI_BINDING_SLOT)]] RaytracingAccelerationStructure tlas;

// 返回两个Texture(ColorAttachment)
struct PSOutput {
    float4 color_with_ao : SV_Target0;
    float4 ambient_only : SV_Target1;
};

static const float Epsilon = 0.0001; // TODO: same with PBRMaterialFrag.hlsl

// TODO: 代码整理
namespace Moer {
typedef Math::Rng::Hash RandomState;
}

// y>=0半球上均匀采样
float3 SampleHemisphere(float2 u) { // uv in [0, 1)^2
    float y = u.x; // cos theta
    float r = sqrt(max(0.f, 1.f - y * y));
    float phi = 2.0 * 3.14159265 * u.y;
    return float3(r * cos(phi), y, r * sin(phi));
}

// y>=0半球上cosine-weighted采样（即 半球正面采样概率大，适合漫反射材质）
float3 SampleCosineHemisphere(float2 u) {
    float r = sqrt(u.x);
    float theta = 2.0 * 3.14159265 * u.y;

    float x = r * cos(theta);
    float y = sqrt(max(0.0, 1.0 - u.x));
    float z = r * sin(theta);
    return float3(x, y, z);
}

// 将一个半球坐标系中的vector转换到以某个特定normal为+z的半球上 (Written by AI)
float3 LocalVectorToWorld(float3 local_vector, float3 normal) {
    float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return local_vector.x * tangent
           + local_vector.y * normal
           + local_vector.z * bitangent;
}

// TODO: 和RT那边的函数合并
bool CastVisibilityRay(float3 origin, float3 direction, float tmin, float tmax,
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

PSOutput main(float2 uv : TEXCOORD0) {

    // Reference: shaders/hwrt/GBufferUtils.hlsli: SetupPrimaryRay()

    float3 color = TextureHandle(param.input_image).Sample2D<float3>(uv);
    float3 frag_normal = TextureHandle(param.normal_tex).Sample2D<float3>(uv);

    if (abs(frag_normal.x) < Epsilon && abs(frag_normal.y) < Epsilon && abs(frag_normal.z) < Epsilon) {
        // direct light is sky
        PSOutput output;
        output.color_with_ao = float4(color, 1.f);
        output.ambient_only = float4(1.f, 1.f, 1.f, 1.f);
        return output;
    }
    frag_normal = Raster::UnpackNormal(frag_normal);

    Moer::RandomState rng = Moer::RandomState::Create(uv * param.resolution, param.frame_idx);

    float3 frag_position = TextureHandle(param.position_tex).Sample2D<float3>(uv);

    // Raytraced AO
    float2 rand_value = rng.GetFloat2();
    float3 rand_vec;
    if (param.sample_mode == 0) {
        rand_vec = SampleHemisphere(rand_value);
    } else {
        rand_vec = SampleCosineHemisphere(rand_value);
    }
    float3 direction = LocalVectorToWorld(rand_vec, frag_normal);
    bool is_sky = CastVisibilityRay(
        frag_position + frag_normal * 0.01,
        direction,
        0.f,
        param.ray_trace_distance,
        tlas,
        Moer::RTVM_ALL, // instance_mask
        RAY_FLAG_NONE   // ray_flags
    );

    // if (uv.x <= param.inv_resolution.x && uv.y <= param.inv_resolution.y) {
    //     printf(
    //         "FragPos (%.2f %.2f %.2f); FragNormal (%.2f %.2f %.2f); RandVa (%.4f %.4f); RandVector (%.4f %.4f %.4f); Direction (%.4f %.4f %.4f); is_sky: %d\n",
    //         frag_position.x,
    //         frag_position.y,
    //         frag_position.z,
    //         frag_normal.x,
    //         frag_normal.y,
    //         frag_normal.z,
    //         rand_value.x,
    //         rand_value.y,
    //         rand_vec.x,
    //         rand_vec.y,
    //         rand_vec.z,
    //         direction.x,
    //         direction.y,
    //         direction.z,
    //         int(is_sky)
    //     );
    // }
    
    float ao = is_sky ? 1.f : 0.0f;

    PSOutput output;
    output.color_with_ao = float4(color * ao, 1.0);
    output.ambient_only = float4(ao, ao, ao, 1.0);
    return output;
}