#include "core/math/Math.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::ProbeUpdateParam> param;

[[vk::binding(0, 0)]] RWStructuredBuffer<Moer::ProbeGridProbeData> rw_probe_data;

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
    const float3 irradiance =
        max((sky_gradient + directional_bounce + ground_bounce + local_color_bounce) * lateral_variation, float3(0.0, 0.0, 0.0));

    Moer::ProbeGridProbeData probe;
    probe.world_position      = float4(position, 1.0);
    probe.irradiance          = float4(irradiance, 1.0);
    rw_probe_data[probe_index] = probe;
}
