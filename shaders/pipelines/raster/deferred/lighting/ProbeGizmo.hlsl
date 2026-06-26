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

float3 ProbeGizmoGetBoundsOrigin() {
    return float3(
        asfloat(param.probe_volume_config.y),
        asfloat(param.probe_volume_config.z),
        asfloat(param.probe_volume_config.w)
    );
}

void ProbeGizmoGetBoundsEdge(uint edge_index, out float3 edge_start, out float3 edge_end) {
    const float3 origin = ProbeGizmoGetBoundsOrigin();
    const float3 extent = max(param.gizmo_config.xyz, float3(0.001, 0.001, 0.001));

    float3 corner_a = float3(0.0, 0.0, 0.0);
    float3 corner_b = float3(0.0, 0.0, 0.0);

    if (edge_index < 4u) {
        const float y = float(edge_index & 1u);
        const float z = float((edge_index >> 1u) & 1u);
        corner_a = float3(0.0, y, z);
        corner_b = float3(1.0, y, z);
    } else if (edge_index < 8u) {
        const uint local_edge = edge_index - 4u;
        const float x = float(local_edge & 1u);
        const float z = float((local_edge >> 1u) & 1u);
        corner_a = float3(x, 0.0, z);
        corner_b = float3(x, 1.0, z);
    } else {
        const uint local_edge = edge_index - 8u;
        const float x = float(local_edge & 1u);
        const float y = float((local_edge >> 1u) & 1u);
        corner_a = float3(x, y, 0.0);
        corner_b = float3(x, y, 1.0);
    }

    edge_start = origin + extent * corner_a;
    edge_end = origin + extent * corner_b;
}

ProbeGizmoVSOutput ProbeVolumeBoundsVS(uint vertex_id) {
    ProbeGizmoVSOutput output = (ProbeGizmoVSOutput)0;

    const uint edge_index = min(vertex_id / 6u, 11u);
    const uint quad_vertex = vertex_id % 6u;

    float3 edge_start;
    float3 edge_end;
    ProbeGizmoGetBoundsEdge(edge_index, edge_start, edge_end);

    const float3 edge_mid = (edge_start + edge_end) * 0.5;
    const float3 edge_dir = ProbeGizmoSafeNormalize(edge_end - edge_start, float3(1.0, 0.0, 0.0));
    const float3 view_dir = ProbeGizmoSafeNormalize(param.camera_position.xyz - edge_mid, float3(0.0, 0.0, 1.0));
    float3 side = cross(edge_dir, view_dir);
    if (dot(side, side) < 1e-6) {
        side = abs(edge_dir.y) > 0.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    }
    side = ProbeGizmoSafeNormalize(side, float3(0.0, 1.0, 0.0));

    const float endpoint_t = (quad_vertex == 0u || quad_vertex == 1u || quad_vertex == 4u) ? 0.0 : 1.0;
    const float side_sign = (quad_vertex == 0u || quad_vertex == 2u || quad_vertex == 3u) ? -1.0 : 1.0;
    const float3 world_position =
        lerp(edge_start, edge_end, endpoint_t) + side * side_sign * max(param.gizmo_config.w, 0.001);

    output.position = mul(param.world2clip, float4(world_position, 1.0));
    output.color = float4(max(param.fixed_color.rgb, float3(0.0, 0.0, 0.0)), param.fixed_color.a);
    return output;
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
    if (param.probe_volume_config.x == Moer::RASTER_PROBE_GIZMO_DRAW_MODE_BOUNDS) {
        return ProbeVolumeBoundsVS(vertex_id);
    }

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
