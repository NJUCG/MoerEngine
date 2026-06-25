#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::ProbeGizmoParam> param;

struct ProbeGizmoVSOutput {
    float4                     position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

float3 ProbeGizmoGetAxis(uint axis_index) {
    if (axis_index == 0u) {
        return float3(1.0, 0.0, 0.0);
    }
    if (axis_index == 1u) {
        return float3(0.0, 1.0, 0.0);
    }
    return float3(0.0, 0.0, 1.0);
}

float3 ProbeGizmoSafeNormalize(float3 value, float3 fallback) {
    float len_sq = dot(value, value);
    if (len_sq < 1e-6) {
        return fallback;
    }
    return value * rsqrt(len_sq);
}

uint ProbeGizmoGetQuadCornerIndex(uint vertex_id) {
    const uint quad_vertex = vertex_id % 6u;
    if (quad_vertex == 0u) {
        return 0u;
    }
    if (quad_vertex == 1u) {
        return 1u;
    }
    if (quad_vertex == 2u) {
        return 2u;
    }
    if (quad_vertex == 3u) {
        return 2u;
    }
    if (quad_vertex == 4u) {
        return 1u;
    }
    return 3u;
}

float3 ProbeGizmoGetAxisSide(float3 axis, float3 probe_position) {
    float3 view_dir = ProbeGizmoSafeNormalize(param.camera_position.xyz - probe_position, float3(0.0, 0.0, 1.0));
    float3 side = cross(axis, view_dir);
    if (dot(side, side) < 1e-6) {
        side = abs(axis.y) > 0.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    }
    return ProbeGizmoSafeNormalize(side, float3(0.0, 1.0, 0.0));
}

float3 ProbeGizmoGetColor(Moer::ProbeGridProbeData probe, uint axis_index) {
    float3 axis_tint = ProbeGizmoGetAxis(axis_index);
    float3 fixed_color = max(param.fixed_color.rgb, float3(0.0, 0.0, 0.0));
    uint probe_state = uint(round(max(probe.world_position.w, 0.0)));

    if (param.probe_volume_config.y == 3u) {
        if (probe_state == Moer::RASTER_PROBE_STATE_INVALID) {
            return float3(1.0, 0.05, 0.02) * param.gizmo_config.y;
        }
        if (probe_state == Moer::RASTER_PROBE_STATE_RELOCATED) {
            return float3(1.0, 0.72, 0.08) * param.gizmo_config.y;
        }
        if (probe_state == Moer::RASTER_PROBE_STATE_NEAR_SURFACE) {
            return float3(0.85, 0.20, 1.0) * param.gizmo_config.y;
        }
        return float3(0.10, 0.95, 0.45) * param.gizmo_config.y;
    }

    if (param.probe_volume_config.y == 1u) {
        float3 irradiance = max(probe.irradiance.rgb * probe.irradiance.a, float3(0.0, 0.0, 0.0));
        float irradiance_peak = max(max(irradiance.r, irradiance.g), max(irradiance.b, 1e-4));
        float3 irradiance_vis = irradiance / (1.0 + irradiance_peak);
        return max(irradiance_vis * param.gizmo_config.y, axis_tint * 0.08);
    }
    if (param.probe_volume_config.y == 2u) {
        float open_ratio = saturate(probe.visibility.z);
        float mean_distance = saturate(probe.visibility.x / 16.0);
        return float3(1.0 - open_ratio, open_ratio, mean_distance) * param.gizmo_config.y;
    }

    float state_tint = probe_state == Moer::RASTER_PROBE_STATE_INVALID ? 0.25 : 1.0;
    return fixed_color * (0.55 + axis_tint * 0.45) * param.gizmo_config.y * state_tint;
}

ProbeGizmoVSOutput ProbeGizmoVS(uint vertex_id : SV_VertexID, uint probe_index : SV_InstanceID) {
    ProbeGizmoVSOutput output = (ProbeGizmoVSOutput)0;

    const uint safe_probe_index = min(probe_index, max(param.probe_volume_config.w, 1u) - 1u);
    ArrayBuffer probe_buffer = ArrayBuffer(param.probe_volume_config.z);
    Moer::ProbeGridProbeData probe = probe_buffer.Load<Moer::ProbeGridProbeData>(safe_probe_index);

    const uint axis_index = vertex_id / 6u;
    const uint corner_index = ProbeGizmoGetQuadCornerIndex(vertex_id);
    const float endpoint_sign = (corner_index == 0u || corner_index == 2u) ? -1.0 : 1.0;
    const float side_sign = (corner_index == 0u || corner_index == 1u) ? -1.0 : 1.0;
    const float3 axis = ProbeGizmoGetAxis(axis_index);
    const float3 side = ProbeGizmoGetAxisSide(axis, probe.world_position.xyz);
    const float3 world_position =
        probe.world_position.xyz + axis * endpoint_sign * param.gizmo_config.x + side * side_sign * param.gizmo_config.z;

    output.position = mul(param.world2clip, float4(world_position, 1.0));
    output.color = float4(ProbeGizmoGetColor(probe, axis_index), param.fixed_color.a);
    return output;
}

float4 ProbeGizmoPS(ProbeGizmoVSOutput input) : SV_TARGET {
    return float4(saturate(input.color.rgb), input.color.a);
}
