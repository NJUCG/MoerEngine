#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3)
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"

#include "core/math/Math.hlsli"

[[vk::push_constant]] ConstantBuffer<Moer::RtaoPipelineBindlessParam> param;

[[vk::binding(0, 0)]] RWTexture2D<float>              rw_ao_only;
[[vk::binding(1, 0)]] RWTexture2D<float2>             rw_camera_mv;
[[vk::binding(2, 0)]] RaytracingAccelerationStructure tlas;

// 定义了AoOutput、CameraMotionVector等函数
#include "pipelines/postprocess/lighting_effects/AoCommon.hlsl"

// R2 quasi-random sequence offsets (generalized golden ratio for 2D)
// phi_2 = 1.32471795724..., alpha1 = 1/phi_2, alpha2 = 1/phi_2^2
static const float2 R2_ALPHA = float2(0.7548776662466927, 0.5698402909980532);

// y>=0半球上均匀采样
float4 SampleHemisphere(float2 u) { // uv in [0, 1)^2
    float y   = u.x;                // cos theta
    float r   = sqrt(max(0.f, 1.f - y * y));
    float phi = PI2 * u.y;
    return float4(r * cos(phi), y, r * sin(phi), /* pdf */ 1.0 / PI2);
}

// y>=0半球上cosine-weighted采样（即 半球正面采样概率大，适合漫反射材质）
float4 SampleCosineHemisphere(float2 u) {
    float r     = sqrt(u.x);
    float theta = 2.0 * PI * u.y;
    float y     = sqrt(max(0.0, 1.0 - u.x));

    return float4(r * cos(theta), y, r * sin(theta), /* pdf */ y / PI);
}

// 将一个半球坐标系中的vector转换到以某个特定normal为z的半球上 (Written by AI)
float3 LocalVectorToWorld(float3 local_vector, float3 normal) {
    float3 up        = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent   = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return local_vector.x * tangent + local_vector.y * normal + local_vector.z * bitangent;
}

bool CastVisibilityRay(
    float3                          origin,
    float3                          direction,
    float                           tmin,
    float                           tmax,
    RaytracingAccelerationStructure accel
) {
    RayDesc ray_desc;
    ray_desc.Origin    = origin;
    ray_desc.Direction = direction;
    ray_desc.TMin      = tmin;
    ray_desc.TMax      = tmax;

    RayQuery<
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_FORCE_OPAQUE>
        ray_query;
    ray_query.TraceRayInline(accel, RAY_FLAG_NONE, Moer::RTVM_ALL, ray_desc);
    ray_query.Proceed();

    return ray_query.CommittedStatus() == COMMITTED_NOTHING;
}

[numthreads(8, 8, 1)] void main(uint2 pixel_pos : SV_DispatchThreadID) {
    if (pixel_pos.x >= uint(param.resolution.x) || pixel_pos.y >= uint(param.resolution.y))
        return;

    // Reference: shaders/hwrt/GBufferUtils.hlsli: SetupPrimaryRay()

    float2 uv = (float2(pixel_pos) + 0.5) * param.inv_resolution;

    // Snap UV to the full-res depth texel center so that WorldPosFromDepth
    // reconstructs along the same view ray the depth was measured on.
    float2 snapped_uv = (floor(uv * param.depth_tex_resolution) + 0.5) / param.depth_tex_resolution;

    float3 frag_normal = TextureHandle(param.normal_tex).SampleLevel<float3>(snapped_uv);

    if (abs(frag_normal.x) < Epsilon && abs(frag_normal.y) < Epsilon && abs(frag_normal.z) < Epsilon) {
        rw_ao_only[pixel_pos]   = 1.f;
        rw_camera_mv[pixel_pos] = GetCameraMotionVector(snapped_uv);
        return;
    }
    frag_normal = Raster::UnpackNormal(frag_normal);

    float  depth         = TextureHandle(param.depth_tex).SampleLevel<float>(snapped_uv);
    float3 frag_position = WorldPosFromDepth(depth, snapped_uv, param.clip2world);

    // Blue noise base value — spatially coherent across neighboring pixels
    float2 noise_uv        = uv * param.resolution / 256.0;
    float2 blue_noise_base = TextureHandle(param.noise_tex).SampleLevel<float2>(noise_uv);

    // Raytraced AO
    float total_ray_contrib   = 0.0;
    float visible_ray_contrib = 0.0;
    for (uint i = 0; i < param.spp; i++) {
        // Cranley-Patterson rotation with R2 quasi-random sequence for temporal + per-sample variation
        float2 rand_value = frac(blue_noise_base + R2_ALPHA * float(param.frame_idx * param.spp + i));
#if RTAO_COSINE_WEIGHTED
        float4 rand_vec = SampleCosineHemisphere(rand_value);
#else
        float4 rand_vec = SampleHemisphere(rand_value);
#endif
        float3 direction  = LocalVectorToWorld(rand_vec.xyz, frag_normal);
        float  ray_weight = max(dot(frag_normal, direction), 0.05f) / max(/* pdf */ rand_vec.w, 0.05f);

        bool is_miss = CastVisibilityRay(
            frag_position + frag_normal * 0.01, direction, 0.f, param.ray_trace_distance, tlas
        );

        total_ray_contrib += ray_weight;
        visible_ray_contrib += ray_weight * (is_miss ? 1.0 : (1.0 - param.intensity));
    }

    float ao = visible_ray_contrib / total_ray_contrib;

    rw_ao_only[pixel_pos]   = ao;
    rw_camera_mv[pixel_pos] = GetCameraMotionVector(snapped_uv);
}
