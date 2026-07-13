#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::CameraGizmoParam> param;

struct CameraGizmoVSOutput {
    float4                     position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

static const uint CAMERA_GIZMO_FRUSTUM_EDGE_COUNT = 16u;
static const uint CAMERA_GIZMO_FRUSTUM_VERTEX_COUNT = CAMERA_GIZMO_FRUSTUM_EDGE_COUNT * 6u;
static const float CAMERA_GIZMO_MIN_CLIP_W = 0.35;
static const float CAMERA_GIZMO_NDC_CLIP_EXTENT = 1.35;

uint2 CameraGizmoGetEdge(uint edge_index) {
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
    if (edge_index == 11u) {
        return uint2(3u, 7u);
    }
    if (edge_index == 12u) {
        return uint2(8u, 0u);
    }
    if (edge_index == 13u) {
        return uint2(8u, 1u);
    }
    if (edge_index == 14u) {
        return uint2(8u, 2u);
    }
    return uint2(8u, 3u);
}

uint CameraGizmoGetQuadCornerIndex(uint vertex_id) {
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

float2 CameraGizmoSafeNormalize(float2 value, float2 fallback) {
    const float len_sq = dot(value, value);
    if (len_sq < 1e-8) {
        return fallback;
    }
    return value * rsqrt(len_sq);
}

bool CameraGizmoClipToMinW(inout float4 start_clip, inout float4 end_clip) {
    const bool start_valid = start_clip.w >= CAMERA_GIZMO_MIN_CLIP_W;
    const bool end_valid = end_clip.w >= CAMERA_GIZMO_MIN_CLIP_W;
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

    const float t = saturate((CAMERA_GIZMO_MIN_CLIP_W - start_clip.w) / denom);
    const float4 clipped = lerp(start_clip, end_clip, t);
    if (!start_valid) {
        start_clip = clipped;
        start_clip.w = max(start_clip.w, CAMERA_GIZMO_MIN_CLIP_W);
    } else {
        end_clip = clipped;
        end_clip.w = max(end_clip.w, CAMERA_GIZMO_MIN_CLIP_W);
    }
    return true;
}

bool CameraGizmoClipTest(float p, float q, inout float t0, inout float t1) {
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

bool CameraGizmoClipToNdcRect(
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
    if (!CameraGizmoClipTest(-delta.x, start_ndc0.x + CAMERA_GIZMO_NDC_CLIP_EXTENT, t0, t1)) {
        return false;
    }
    if (!CameraGizmoClipTest(delta.x, CAMERA_GIZMO_NDC_CLIP_EXTENT - start_ndc0.x, t0, t1)) {
        return false;
    }
    if (!CameraGizmoClipTest(-delta.y, start_ndc0.y + CAMERA_GIZMO_NDC_CLIP_EXTENT, t0, t1)) {
        return false;
    }
    if (!CameraGizmoClipTest(delta.y, CAMERA_GIZMO_NDC_CLIP_EXTENT - start_ndc0.y, t0, t1)) {
        return false;
    }

    start_ndc = lerp(start_ndc0, end_ndc0, t0);
    end_ndc = lerp(start_ndc0, end_ndc0, t1);
    start_clip = lerp(start_clip0, end_clip0, t0);
    end_clip = lerp(start_clip0, end_clip0, t1);
    return dot(end_ndc - start_ndc, end_ndc - start_ndc) > 1e-8;
}

CameraGizmoVSOutput CameraGizmoHiddenVS() {
    CameraGizmoVSOutput output = (CameraGizmoVSOutput)0;
    output.position = float4(2.0, 2.0, 0.0, 1.0);
    output.color = float4(0.0, 0.0, 0.0, 0.0);
    return output;
}

float3 CameraGizmoGetCorner(uint corner_index) {
    if (corner_index == 8u) {
        return param.camera_position_near.xyz;
    }

    const bool is_far = corner_index >= 4u;
    const uint local_index = corner_index & 3u;

    const float depth = is_far ? param.camera_front_far.w : param.camera_position_near.w;
    const float half_height = depth * param.camera_right_tan_half_fov.w;
    const float half_width = half_height * param.camera_up_aspect.w;
    const float right_sign = (local_index == 0u || local_index == 2u) ? 1.0 : -1.0;
    const float up_sign = (local_index < 2u) ? 1.0 : -1.0;

    return param.camera_position_near.xyz +
           param.camera_front_far.xyz * depth +
           param.camera_right_tan_half_fov.xyz * right_sign * half_width +
           param.camera_up_aspect.xyz * up_sign * half_height;
}

CameraGizmoVSOutput CameraGizmoLineVS(uint vertex_id) {
    CameraGizmoVSOutput output = (CameraGizmoVSOutput)0;

    const uint edge_index = min(vertex_id / 6u, CAMERA_GIZMO_FRUSTUM_EDGE_COUNT - 1u);
    const uint quad_vertex = vertex_id % 6u;
    const uint2 edge = CameraGizmoGetEdge(edge_index);

    float4 start_clip = mul(param.world2clip, float4(CameraGizmoGetCorner(edge.x), 1.0));
    float4 end_clip = mul(param.world2clip, float4(CameraGizmoGetCorner(edge.y), 1.0));
    if (!CameraGizmoClipToMinW(start_clip, end_clip)) {
        return CameraGizmoHiddenVS();
    }

    float2 start_ndc = start_clip.xy / start_clip.w;
    float2 end_ndc = end_clip.xy / end_clip.w;
    if (!CameraGizmoClipToNdcRect(start_clip, end_clip, start_ndc, end_ndc)) {
        return CameraGizmoHiddenVS();
    }

    const float2 edge_dir = CameraGizmoSafeNormalize(end_ndc - start_ndc, float2(1.0, 0.0));
    const float2 side = float2(-edge_dir.y, edge_dir.x);
    const float endpoint_t = (quad_vertex == 0u || quad_vertex == 1u || quad_vertex == 4u) ? 0.0 : 1.0;
    const float side_sign = (quad_vertex == 0u || quad_vertex == 2u || quad_vertex == 3u) ? -1.0 : 1.0;

    const float line_thickness =
        edge_index < 4u ? 0.0052 :
        (edge_index >= 12u ? 0.0048 : 0.0042);
    const float4 base_clip = lerp(start_clip, end_clip, endpoint_t);
    const float2 base_ndc = lerp(start_ndc, end_ndc, endpoint_t);
    const float2 thick_ndc = base_ndc + side * side_sign * line_thickness;

    output.position = base_clip;
    output.position.xy = thick_ndc * base_clip.w;
    output.color =
        edge_index < 4u ? float4(1.0, 0.78, 0.14, 0.74) :
        (edge_index >= 12u ? float4(1.0, 0.62, 0.10, 0.68) : float4(0.15, 0.48, 1.0, 0.58));
    return output;
}

CameraGizmoVSOutput CameraGizmoIconVS(uint icon_vertex_id) {
    CameraGizmoVSOutput output = (CameraGizmoVSOutput)0;

    const uint part = min(icon_vertex_id / 6u, 2u);
    const uint corner_index = CameraGizmoGetQuadCornerIndex(icon_vertex_id);
    const float2 rect_corner = float2(
        (corner_index == 0u || corner_index == 2u) ? -1.0 : 1.0,
        (corner_index == 0u || corner_index == 1u) ? -1.0 : 1.0
    );

    float2 center = float2(0.0, 0.0);
    float2 half_extent = float2(0.026, 0.016);
    float4 color = float4(1.0, 0.78, 0.14, 0.74);

    if (part == 1u) {
        center = float2(0.0, 0.0);
        half_extent = float2(0.009, 0.009);
        color = float4(0.15, 0.56, 1.0, 0.72);
    } else if (part == 2u) {
        center = float2(-0.010, 0.018);
        half_extent = float2(0.010, 0.006);
        color = float4(1.0, 0.58, 0.10, 0.68);
    }

    const float4 center_clip = mul(param.world2clip, float4(param.camera_position_near.xyz, 1.0));
    if (center_clip.w < CAMERA_GIZMO_MIN_CLIP_W) {
        return CameraGizmoHiddenVS();
    }

    const float2 center_ndc = center_clip.xy / center_clip.w;
    if (any(abs(center_ndc) > CAMERA_GIZMO_NDC_CLIP_EXTENT)) {
        return CameraGizmoHiddenVS();
    }

    const float2 icon_ndc = center_ndc + center + rect_corner * half_extent;

    output.position = center_clip;
    output.position.xy = icon_ndc * center_clip.w;
    output.color = color;
    return output;
}

CameraGizmoVSOutput CameraGizmoVS(uint vertex_id : SV_VertexID) {
    if (vertex_id < CAMERA_GIZMO_FRUSTUM_VERTEX_COUNT) {
        return CameraGizmoLineVS(vertex_id);
    }
    return CameraGizmoIconVS(vertex_id - CAMERA_GIZMO_FRUSTUM_VERTEX_COUNT);
}

float4 CameraGizmoPS(CameraGizmoVSOutput input) : SV_TARGET {
    return input.color;
}
