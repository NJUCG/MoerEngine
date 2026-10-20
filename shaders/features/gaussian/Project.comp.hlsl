#include "features/gaussian/GaussianMath.hlsli"

[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] ConstantBuffer<Moer::GGaussianCamera> camera;
[[vk::binding(1, 0)]] StructuredBuffer<Moer::GGaussian> gaussians;
[[vk::binding(2, 0)]] StructuredBuffer<Moer::GGaussianInstance> instances;
[[vk::binding(3, 0)]] RWStructuredBuffer<Moer::GProjectedGaussian> projected;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> tile_counts;

[numthreads(256, 1, 1)]
void main(uint3 thread_id : SV_DispatchThreadID) {
    uint local_id = thread_id.x;
    if (local_id >= param.job.w) return;
    uint work_id = param.job.z + local_id;
    projected[work_id] = (Moer::GProjectedGaussian)0;
    tile_counts[work_id] = 0;
    Moer::GGaussian gaussian = gaussians[param.job.x + local_id];
    Moer::GGaussianInstance instance = instances[param.job.y];
    float3 world_center = mul(instance.world_transform, float4(gaussian.position.xyz, 1)).xyz;
    float4 view_center = mul(camera.world_to_view, float4(world_center, 1));
    float depth = -view_center.z;
    if (!all(isfinite(view_center)) || depth <= camera.filter.z || depth >= camera.filter.w) return;
    float4 clip = mul(camera.view_to_clip, view_center);
    if (!all(isfinite(clip)) || clip.w <= 0) return;
    float2 pixel_scale = float2(camera.viewport.x, -float(camera.viewport.y)) * 0.5;
    float2 center = (clip.xy / clip.w) * pixel_scale + float2(camera.viewport.xy) * 0.5 - 0.5;

    float3x3 linear_world = (float3x3)instance.world_transform;
    float determinant_world = dot(linear_world[0], cross(linear_world[1], linear_world[2]));
    if (!isfinite(determinant_world) || abs(determinant_world) < 1e-12) return;
    float3x3 inverse_world = transpose(float3x3(cross(linear_world[1], linear_world[2]),
        cross(linear_world[2], linear_world[0]), cross(linear_world[0], linear_world[1]))) / determinant_world;
    float3 direction = normalize(mul(inverse_world, world_center - camera.position.xyz));
    float3x3 rotation = GaussianRotation(gaussian.rotation);
    float3 scale = gaussian.scale_opacity.xyz;
    float3x3 axes = mul(mul((float3x3)camera.world_to_view, linear_world), rotation);
    axes[0] *= scale; axes[1] *= scale; axes[2] *= scale;

    // Clamp the covariance linearization point, while retaining the actual projected center.
    float4 covariance_center = view_center;
    covariance_center.x = clamp(view_center.x / depth, -1.3 / abs(camera.view_to_clip[0][0]), 1.3 / abs(camera.view_to_clip[0][0])) * depth;
    covariance_center.y = clamp(view_center.y / depth, -1.3 / abs(camera.view_to_clip[1][1]), 1.3 / abs(camera.view_to_clip[1][1])) * depth;
    float4 covariance_clip = mul(camera.view_to_clip, covariance_center);
    float3 gradient_x = pixel_scale.x * (camera.view_to_clip[0].xyz * covariance_clip.w - covariance_clip.x * camera.view_to_clip[3].xyz) / (covariance_clip.w * covariance_clip.w);
    float3 gradient_y = pixel_scale.y * (camera.view_to_clip[1].xyz * covariance_clip.w - covariance_clip.y * camera.view_to_clip[3].xyz) / (covariance_clip.w * covariance_clip.w);
    float3 axis_x = mul(gradient_x, axes), axis_y = mul(gradient_y, axes);
    float a = dot(axis_x, axis_x) + camera.filter.x;
    float b = dot(axis_x, axis_y);
    float c = dot(axis_y, axis_y) + camera.filter.x;
    float determinant_cov = a*c - b*b;
    if (!isfinite(determinant_cov) || determinant_cov <= 0) return;
    float mid = 0.5 * (a+c);
    float radius = ceil(3 * sqrt(mid + sqrt(max(0.1, mid*mid - determinant_cov))));
    if (!isfinite(radius) || !all(isfinite(center))) return;
    float2 tile_min = clamp(floor((center - radius) / 16), 0, float2(camera.viewport.zw));
    float2 tile_max = clamp(ceil((center + radius) / 16), 0, float2(camera.viewport.zw));
    uint4 rect = uint4(uint2(tile_min), uint2(tile_max));
    float opacity = gaussian.scale_opacity.w * instance.color_opacity.w;
    if (opacity < camera.filter.y || any(rect.zw <= rect.xy)) return;
    Moer::GProjectedGaussian output;
    output.center_depth = float4(center, depth, 0);
    output.conic_opacity = float4(c, -b, a, opacity) / float4(determinant_cov.xxx, 1);
    output.color = float4(GaussianSH(gaussian, direction, min(instance.range.z, 3u)) * instance.color_opacity.xyz, 0);
    output.tile_rect = rect;
    projected[work_id] = output;
    tile_counts[work_id] = (rect.z - rect.x) * (rect.w - rect.y);
}
