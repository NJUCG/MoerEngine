#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::CsmGizmoParam> param;

struct CsmGizmoVSOutput {
    float4                     position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

static const uint CSM_GIZMO_VERTICES_PER_EDGE = 6u;
static const uint CSM_GIZMO_FRUSTUM_EDGE_COUNT = 12u;
static const uint CSM_GIZMO_FRUSTUM_VERTEX_COUNT =
    CSM_GIZMO_FRUSTUM_EDGE_COUNT * CSM_GIZMO_VERTICES_PER_EDGE;
static const uint CSM_GIZMO_SPHERE_SEGMENT_COUNT = 32u;
static const uint CSM_GIZMO_SPHERE_CIRCLE_COUNT = 3u;
static const uint CSM_GIZMO_SPHERE_VERTEX_COUNT =
    CSM_GIZMO_SPHERE_SEGMENT_COUNT * CSM_GIZMO_SPHERE_CIRCLE_COUNT * CSM_GIZMO_VERTICES_PER_EDGE;
static const float CSM_GIZMO_MIN_CLIP_W = 0.04;
static const float CSM_GIZMO_NDC_CLIP_EXTENT = 6.0;

float2 CsmGizmoSafeNormalize2(float2 value, float2 fallback) {
    const float len_sq = dot(value, value);
    if (len_sq < 1e-8) {
        return fallback;
    }
    return value * rsqrt(len_sq);
}

uint2 CsmGizmoGetFrustumEdge(uint edge_index) {
    if (edge_index == 0u) {
        return uint2(0u, 1u);
    }
    if (edge_index == 1u) {
        return uint2(0u, 2u);
    }
    if (edge_index == 2u) {
        return uint2(2u, 3u);
    }
    if (edge_index == 3u) {
        return uint2(1u, 3u);
    }
    if (edge_index == 4u) {
        return uint2(4u, 5u);
    }
    if (edge_index == 5u) {
        return uint2(4u, 6u);
    }
    if (edge_index == 6u) {
        return uint2(6u, 7u);
    }
    if (edge_index == 7u) {
        return uint2(5u, 7u);
    }
    if (edge_index == 8u) {
        return uint2(0u, 4u);
    }
    if (edge_index == 9u) {
        return uint2(1u, 5u);
    }
    if (edge_index == 10u) {
        return uint2(2u, 6u);
    }
    return uint2(3u, 7u);
}

float3 CsmGizmoGetSpherePoint(Moer::CsmGizmoCascadeData cascade_data, uint circle_index, float angle) {
    const float s = sin(angle);
    const float c = cos(angle);
    const float3 center = cascade_data.sphere_center_radius.xyz;
    const float radius = max(cascade_data.sphere_center_radius.w, 1e-4);

    if (circle_index == 0u) {
        return center + float3(c * radius, s * radius, 0.0);
    }
    if (circle_index == 1u) {
        return center + float3(c * radius, 0.0, s * radius);
    }
    return center + float3(0.0, c * radius, s * radius);
}

bool CsmGizmoClipToMinW(inout float4 start_clip, inout float4 end_clip) {
    const bool start_valid = start_clip.w >= CSM_GIZMO_MIN_CLIP_W;
    const bool end_valid = end_clip.w >= CSM_GIZMO_MIN_CLIP_W;
    if (!start_valid && !end_valid) {
        return false;
    }
    if (start_valid && end_valid) {
        return true;
    }

    const float denom = end_clip.w - start_clip.w;
    if (abs(denom) < 1e-5) {
        return false;
    }

    const float t = saturate((CSM_GIZMO_MIN_CLIP_W - start_clip.w) / denom);
    const float4 clipped = lerp(start_clip, end_clip, t);
    if (!start_valid) {
        start_clip = clipped;
        start_clip.w = max(start_clip.w, CSM_GIZMO_MIN_CLIP_W);
    } else {
        end_clip = clipped;
        end_clip.w = max(end_clip.w, CSM_GIZMO_MIN_CLIP_W);
    }
    return true;
}

bool CsmGizmoClipTest(float p, float q, inout float t0, inout float t1) {
    if (abs(p) < 1e-6) {
        return q >= 0.0;
    }

    const float r = q / p;
    if (p < 0.0) {
        if (r > t1) {
            return false;
        }
        if (r > t0) {
            t0 = r;
        }
    } else {
        if (r < t0) {
            return false;
        }
        if (r < t1) {
            t1 = r;
        }
    }
    return true;
}

bool CsmGizmoClipToNdcRect(
    inout float4 start_clip,
    inout float4 end_clip,
    inout float2 start_ndc,
    inout float2 end_ndc
) {
    const float2 start_ndc0 = start_ndc;
    const float2 end_ndc0 = end_ndc;
    const float4 start_clip0 = start_clip;
    const float4 end_clip0 = end_clip;
    const float2 delta = end_ndc0 - start_ndc0;

    float t0 = 0.0;
    float t1 = 1.0;
    if (!CsmGizmoClipTest(-delta.x, start_ndc0.x + CSM_GIZMO_NDC_CLIP_EXTENT, t0, t1)) {
        return false;
    }
    if (!CsmGizmoClipTest(delta.x, CSM_GIZMO_NDC_CLIP_EXTENT - start_ndc0.x, t0, t1)) {
        return false;
    }
    if (!CsmGizmoClipTest(-delta.y, start_ndc0.y + CSM_GIZMO_NDC_CLIP_EXTENT, t0, t1)) {
        return false;
    }
    if (!CsmGizmoClipTest(delta.y, CSM_GIZMO_NDC_CLIP_EXTENT - start_ndc0.y, t0, t1)) {
        return false;
    }

    start_ndc = lerp(start_ndc0, end_ndc0, t0);
    end_ndc = lerp(start_ndc0, end_ndc0, t1);
    start_clip = lerp(start_clip0, end_clip0, t0);
    end_clip = lerp(start_clip0, end_clip0, t1);
    return dot(end_ndc - start_ndc, end_ndc - start_ndc) > 1e-8;
}

CsmGizmoVSOutput CsmGizmoHiddenLineVS() {
    CsmGizmoVSOutput output = (CsmGizmoVSOutput)0;
    output.position = float4(2.0, 2.0, 0.0, 1.0);
    output.color = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}

CsmGizmoVSOutput CsmGizmoLineVS(float3 edge_start, float3 edge_end, float4 color, uint vertex_id) {
    CsmGizmoVSOutput output = (CsmGizmoVSOutput)0;

    const uint quad_vertex = vertex_id % CSM_GIZMO_VERTICES_PER_EDGE;
    const float endpoint_t = (quad_vertex == 0u || quad_vertex == 1u || quad_vertex == 4u) ? 0.0 : 1.0;
    const float side_sign = (quad_vertex == 0u || quad_vertex == 2u || quad_vertex == 3u) ? -1.0 : 1.0;

    float4 start_clip = mul(param.world2clip, float4(edge_start, 1.0));
    float4 end_clip = mul(param.world2clip, float4(edge_end, 1.0));
    if (!CsmGizmoClipToMinW(start_clip, end_clip)) {
        return CsmGizmoHiddenLineVS();
    }

    float2 start_ndc = start_clip.xy / start_clip.w;
    float2 end_ndc = end_clip.xy / end_clip.w;
    if (!CsmGizmoClipToNdcRect(start_clip, end_clip, start_ndc, end_ndc)) {
        return CsmGizmoHiddenLineVS();
    }

    const float2 edge_dir = CsmGizmoSafeNormalize2(end_ndc - start_ndc, float2(1.0, 0.0));
    const float2 side = float2(-edge_dir.y, edge_dir.x);
    const float thickness = clamp(param.camera_position_thickness.w, 0.0005, 0.014);

    const float4 base_clip = lerp(start_clip, end_clip, endpoint_t);
    const float2 base_ndc = lerp(start_ndc, end_ndc, endpoint_t);
    const float2 thick_ndc = base_ndc + side * side_sign * thickness;

    output.position = base_clip;
    output.position.xy = thick_ndc * base_clip.w;
    output.color = color;
    return output;
}

CsmGizmoVSOutput CsmGizmoFrustumVS(Moer::CsmGizmoCascadeData cascade_data, uint vertex_id) {
    const uint edge_index = min(vertex_id / CSM_GIZMO_VERTICES_PER_EDGE, CSM_GIZMO_FRUSTUM_EDGE_COUNT - 1u);
    const uint2 edge = CsmGizmoGetFrustumEdge(edge_index);
    return CsmGizmoLineVS(
        cascade_data.frustum_corners[edge.x].xyz,
        cascade_data.frustum_corners[edge.y].xyz,
        cascade_data.color,
        vertex_id
    );
}

CsmGizmoVSOutput CsmGizmoSphereVS(Moer::CsmGizmoCascadeData cascade_data, uint vertex_id) {
    const uint edge_index = vertex_id / CSM_GIZMO_VERTICES_PER_EDGE;
    const uint circle_index = min(edge_index / CSM_GIZMO_SPHERE_SEGMENT_COUNT, CSM_GIZMO_SPHERE_CIRCLE_COUNT - 1u);
    const uint segment_index = edge_index % CSM_GIZMO_SPHERE_SEGMENT_COUNT;

    const float angle0 = (float(segment_index) / float(CSM_GIZMO_SPHERE_SEGMENT_COUNT)) * 6.28318530718;
    const float angle1 = (float(segment_index + 1u) / float(CSM_GIZMO_SPHERE_SEGMENT_COUNT)) * 6.28318530718;
    return CsmGizmoLineVS(
        CsmGizmoGetSpherePoint(cascade_data, circle_index, angle0),
        CsmGizmoGetSpherePoint(cascade_data, circle_index, angle1),
        float4(cascade_data.color.rgb, cascade_data.color.a * 0.78),
        vertex_id
    );
}

CsmGizmoVSOutput CsmGizmoVS(uint vertex_id : SV_VertexID, uint cascade_index : SV_InstanceID) {
    const uint cascade_count = max(param.csm_config.y, 1u);
    const uint safe_cascade_index = min(cascade_index, cascade_count - 1u);
    ArrayBuffer cascade_buffer = ArrayBuffer(param.csm_config.x);
    Moer::CsmGizmoCascadeData cascade_data =
        cascade_buffer.Load<Moer::CsmGizmoCascadeData>(safe_cascade_index);

    const bool draw_frustums =
        (param.csm_config.z & Moer::RASTER_CSM_GIZMO_DRAW_SPLIT_FRUSTUM) != 0u;
    const uint frustum_vertex_count = draw_frustums ? CSM_GIZMO_FRUSTUM_VERTEX_COUNT : 0u;
    if (vertex_id < frustum_vertex_count) {
        return CsmGizmoFrustumVS(cascade_data, vertex_id);
    }

    return CsmGizmoSphereVS(cascade_data, vertex_id - frustum_vertex_count);
}

float4 CsmGizmoPS(CsmGizmoVSOutput input) : SV_TARGET {
    return input.color;
}
