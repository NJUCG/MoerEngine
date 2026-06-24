#include "core/math/Math.hlsli"
#include "shared/ShaderParameters.h"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::ProbeUpdateParam> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::ProbeGridProbeData> rw_probe_data;

#if PROBE_GI_USE_RAY_QUERY
[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas;
#endif

static const uint PROBE_GI_MAX_TRACE_RAYS = 32u;

float ProbeSafeSaturate(float value) {
    return saturate(value);
}

float3 ProbeGetGridCoord01(uint3 coord, uint3 counts) {
    return float3(
        counts.x > 1u ? float(coord.x) / float(counts.x - 1u) : 0.5,
        counts.y > 1u ? float(coord.y) / float(counts.y - 1u) : 0.5,
        counts.z > 1u ? float(coord.z) / float(counts.z - 1u) : 0.5
    );
}

float3 ProbeFibonacciDirection(uint sample_index, uint sample_count, float temporal_phase) {
    const float sample_count_f = max(float(sample_count), 1.0);
    const float sample_id = float(sample_index);
    const float z = 1.0 - 2.0 * ((sample_id + 0.5) / sample_count_f);
    const float radius = sqrt(max(0.0, 1.0 - z * z));
    const float phi = (sample_id + temporal_phase * sample_count_f) * 2.39996323;
    return float3(cos(phi) * radius, z, sin(phi) * radius);
}

float3 ProbeEstimateMissRadiance(float3 direction, float3 sun_bounce) {
    const float sky_weight = saturate(direction.y * 0.5 + 0.5);
    const float3 sky_ground =
        lerp(param.probe_ground_color.rgb * 0.55, param.probe_sky_color.rgb, sky_weight) * param.probe_sky_color.a;
    const float sun_alignment = pow(saturate(dot(direction, -param.main_light_direction.xyz)), 48.0);
    return sky_ground + sun_bounce * sun_alignment;
}

#if PROBE_GI_USE_RAY_QUERY
void ProbeTraceVisibilityRay(float3 origin, float3 direction, float tmin, float tmax, out float hit_distance, out float visible) {
    RayDesc ray_desc;
    ray_desc.Origin = origin;
    ray_desc.Direction = direction;
    ray_desc.TMin = tmin;
    ray_desc.TMax = tmax;

    RayQuery<
        RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_FORCE_OPAQUE>
        ray_query;
    ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, Moer::RTVM_ALL, ray_desc);
    ray_query.Proceed();

    if (ray_query.CommittedStatus() == COMMITTED_NOTHING) {
        hit_distance = tmax;
        visible = 1.0;
    } else {
        hit_distance = max(ray_query.CommittedRayT(), tmin);
        visible = 0.0;
    }
}
#else
void ProbeTraceVisibilityRay(float3 origin, float3 direction, float tmin, float tmax, out float hit_distance, out float visible) {
    hit_distance = tmax;
    visible = 1.0;
}
#endif

[numthreads(64, 1, 1)] void main(uint probe_index : SV_DispatchThreadID) {
    const uint total_probe_count = param.probe_volume_counts.w;
    if (probe_index >= total_probe_count) {
        return;
    }

    const uint3 counts = max(param.probe_volume_counts.xyz, uint3(1, 1, 1));
    const uint  x      = probe_index % counts.x;
    const uint  yz     = probe_index / counts.x;
    const uint  y      = yz % counts.y;
    const uint  z      = yz / counts.y;
    const uint3 coord  = uint3(x, y, z);

    const float3 coord01  = ProbeGetGridCoord01(coord, counts);
    const float3 position = param.probe_volume_origin.xyz + param.probe_volume_spacing.xyz * float3(coord);

    const float3 up        = float3(0.0, 1.0, 0.0);
    const float  sun_lift  = ProbeSafeSaturate(dot(-param.main_light_direction.xyz, up));
    const float  direct_light_intensity = max(param.main_light_color.a, 0.0);
    const float  direct_light_energy    = direct_light_intensity / (1.0 + direct_light_intensity);
    const float3 sun_bounce =
        param.main_light_color.rgb * direct_light_energy * param.probe_ground_color.w * (0.35 + 0.65 * sun_lift);

    const float lateral_variation =
        0.88 + 0.12 * sin((coord01.x * 3.17 + coord01.z * 2.41 + param.main_light_direction.w) * PI);
    const float low_volume_weight = pow(saturate(1.0 - coord01.y), 1.35);
    const float wall_bounce_mask = saturate(abs(coord01.x - 0.5) * 2.0) * saturate(1.0 - coord01.y * 0.75);

    const float3 sky_gradient =
        lerp(param.probe_ground_color.rgb * 0.70, param.probe_sky_color.rgb * 0.85, coord01.y) * param.probe_sky_color.a;
    const float3 directional_bounce = sun_bounce * (0.25 + 0.55 * low_volume_weight);
    const float3 local_color_bounce =
        lerp(float3(0.65, 0.26, 0.17), float3(0.12, 0.42, 0.72), smoothstep(0.0, 1.0, coord01.x)) *
        wall_bounce_mask * param.probe_sky_color.a * 0.16;
    const float3 ground_bounce = param.probe_ground_color.rgb * low_volume_weight * param.probe_sky_color.a * 0.30;

    const float max_trace_distance = max(param.probe_trace_config.x, 0.1);
    const float trace_bias = max(param.probe_trace_config.y, 0.02);
    const uint ray_count = clamp(uint(param.probe_trace_config.z), 1u, PROBE_GI_MAX_TRACE_RAYS);

    float  distance_sum = 0.0;
    float  distance_sq_sum = 0.0;
    float  open_sum = 0.0;
    float3 ray_radiance_sum = float3(0.0, 0.0, 0.0);

    [loop] for (uint ray_index = 0u; ray_index < PROBE_GI_MAX_TRACE_RAYS; ++ray_index) {
        if (ray_index >= ray_count) {
            break;
        }

        const float3 ray_direction = ProbeFibonacciDirection(ray_index, ray_count, param.main_light_direction.w);
        float hit_distance = max_trace_distance;
        float visible = 1.0;
        ProbeTraceVisibilityRay(
            position + ray_direction * trace_bias,
            ray_direction,
            0.0,
            max_trace_distance,
            hit_distance,
            visible
        );

        const float normalized_hit = saturate(hit_distance / max_trace_distance);
        const float3 miss_radiance = ProbeEstimateMissRadiance(ray_direction, sun_bounce);
        const float3 hit_bounce = (ground_bounce * 0.45 + local_color_bounce * 1.25 + directional_bounce * 0.20) *
                                  (0.25 + 0.75 * normalized_hit);

        ray_radiance_sum += lerp(hit_bounce, miss_radiance, visible);
        distance_sum += hit_distance;
        distance_sq_sum += hit_distance * hit_distance;
        open_sum += visible;
    }

    const float inv_ray_count = 1.0 / float(ray_count);
    const float mean_distance = distance_sum * inv_ray_count;
    const float mean_distance_sq = distance_sq_sum * inv_ray_count;
    const float open_ratio = open_sum * inv_ray_count;

    const float3 fallback_irradiance =
        max((sky_gradient + directional_bounce + ground_bounce + local_color_bounce) * lateral_variation, float3(0.0, 0.0, 0.0));
    const float3 traced_irradiance =
        ray_radiance_sum * inv_ray_count + directional_bounce * 0.25 + local_color_bounce * (1.0 - open_ratio) * 0.70;
    const float trace_blend = saturate(param.probe_trace_config.w);
    const float3 irradiance = lerp(fallback_irradiance, traced_irradiance, trace_blend);

    Moer::ProbeGridProbeData probe;
    probe.world_position      = float4(position, 1.0);
    probe.irradiance          = float4(irradiance, 1.0);
    probe.visibility          = float4(mean_distance, mean_distance_sq, open_ratio, trace_blend);
    rw_probe_data[probe_index] = probe;
}
