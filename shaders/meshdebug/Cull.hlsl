#include "framework/Common.hlsl"
struct Culler {

  bool b_visible;
  bool b_occluded;
  bool b_cross_nearz;
  void Init() {
    b_visible = true;
    b_occluded = false;
    b_cross_nearz = false;
  }
  bool CullFrustum(in VirtualView view, in float3 center, in float3 extent) {
    float3 min_pos = center - extent;
    float3 max_pos = center + extent;

    // plane tests
    [unroll] for (uint i = 0; i < 6; i++) {
      float3 plane_normal = view.planes[i].xyz;
      float3 candidate_point =
          float3(plane_normal.x > 0 ? max_pos.x : min_pos.x,
                 plane_normal.y > 0 ? max_pos.y : min_pos.y,
                 plane_normal.z > 0 ? max_pos.z : min_pos.z);

      b_visible &= !(dot(float4(candidate_point, 1.0f), view.planes[i]) < 0);
    }
    return b_visible;
  }

  bool CullFrustum(in VirtualView view, in float3 src_center, float src_radius,
                   in float4x4 local_2_world, float scale) {
    float4 center = mul(local_2_world, float4(src_center, 1.0f));
    float radius = src_radius * scale;

    [unroll] for (uint i = 0; i < 6; i++) {
      b_visible &= !(dot(center, view.planes[i]) + radius < 0);
    }
    return b_visible;
  }
  void CalcScreenRect(in float4x4 view_proj, in float3 center, in float3 extent,
                      inout float4 rect, inout float min_z) {
    float3 min_pos = center - extent;
    float3 max_pos = center + extent;

    for (uint i = 0; i < 8; i++) {
      float4 clip = mul(view_proj, float4(corners[i] * extent + center, 1.0f));
      float4 clip_pos = clip / clip.w;
      rect.xy = min(rect.xy, clip_pos.xy);
      rect.zw = max(rect.zw, clip_pos.xy);
      min_z = max(min_z, clip_pos.z); // get front z
      b_cross_nearz |= clip_pos.z < 0;
    }

    rect = saturate(rect * 0.5f + 0.5f); // to [0, 1]
  }
  void CalcScreenSphere(in VirtualView view, in float3 sphere_center,
                        float radius, inout float4 rect, inout float min_z,
                        in float4x4 local_2_proj, float r_scale) {
    float4 center = mul(local_2_proj, float4(sphere_center, 1.0f));
    float z = center.w;
    float inv_z = 1.f / z;
    center /= center.w;
    // get center relative radius
    float world_radius =
        radius * r_scale; // assume this is view space radius too
    if (z - world_radius < view.nearz || z <= 0.f) {
      b_cross_nearz = true;
      return;
    }
    float inv_tan_half_fov = view.inv_tan_half_fov;
    float inv_tan_half_fov_x_aspect = inv_tan_half_fov / view.aspect_ratio;
    float clip_y =
        world_radius * sqrt(inv_tan_half_fov * inv_tan_half_fov + 1.f) * inv_z;
    float clip_x =
        world_radius *
        sqrt(inv_tan_half_fov_x_aspect * inv_tan_half_fov_x_aspect + 1.f) *
        inv_z;

    rect = float4(center.xy - float2(clip_x, clip_y),
                  center.xy + float2(clip_x, clip_y));
    min_z = mul(view.proj, float4(0, 0, world_radius - z, 1.f)).z /
            (z - world_radius);          // reverse depth
    rect = saturate(rect * 0.5f + 0.5f); // to [0, 1]
  }
  bool CullHZB(in float4x4 view_proj, in Texture2D<float> hiz_buffer,
               in SamplerState default_sampler, in float3 center,
               in float3 extent, float2 hiz_factor, float hiz_level) {
    if (!b_visible)
      return false;
    float4 rect = float4(1.f, 1.f, 0.f, 0.f);
    float min_z = 1.f;
    CalcScreenRect(view_proj, center, extent, rect, min_z);
    if (b_cross_nearz)
      return b_visible;
    CullHZBInner(rect, min_z, hiz_buffer, default_sampler, hiz_factor,
                 hiz_level);
    return b_visible && !b_occluded;
  }

  bool CullHZB(in VirtualView view, in Texture2D<float> hiz_buffer,
               in SamplerState default_sampler, in float3 center,
               in float radius, in float4x4 local_2_proj, float scale,
               float2 hiz_factor, float hiz_level) {
    if (!b_visible)
      return false;
    float4 rect = float4(1.f, 1.f, 0.f, 0.f);
    float min_z = 1.f;
    CalcScreenSphere(view, center, radius, rect, min_z, local_2_proj, scale);
    if (b_cross_nearz)
      return b_visible;
    CullHZBInner(rect, min_z, hiz_buffer, default_sampler, hiz_factor,
                 hiz_level);
    return b_visible && !b_occluded;
  }

  void CullHZBInner(in float4 rect, float min_z, in Texture2D<float> hiz_depth,
                    in SamplerState m_sampler, float2 hiz_factor,
                    float hiz_level) {

    float2 hiz_size = (rect.zw - rect.xy) * hiz_factor; // rect size in hiz
    float hiz_mip = log2(max(hiz_size.x, hiz_size.y));  // mip level
    if (hiz_mip > hiz_level) {
      b_occluded = false;
      return;
    }
    hiz_mip = ceil(hiz_mip);
    // sample lower level if it cross less than 2 pixel rect
    float lower_level = max(0.f, hiz_mip - 1.f);
    float2 scale = exp2(-lower_level) * hiz_factor;
    float2 lower_min_xy = floor(rect.xy * scale);
    float2 lower_max_xy = ceil(rect.zw * scale);

    float2 lower_extent = (lower_max_xy - lower_min_xy);
    hiz_mip =
        max(lower_extent.x, lower_extent.y) <= 2.01f ? lower_level : hiz_mip;

    float4 depth_quad =
        float4(hiz_depth.SampleLevel(m_sampler, rect.xy, hiz_mip),
               hiz_depth.SampleLevel(m_sampler, rect.zy, hiz_mip),
               hiz_depth.SampleLevel(m_sampler, rect.xw, hiz_mip),
               hiz_depth.SampleLevel(m_sampler, rect.zw, hiz_mip));

    depth_quad.xy = min(depth_quad.xy, depth_quad.zw);
    depth_quad.x = min(depth_quad.x, depth_quad.y);
    b_occluded = min_z < depth_quad.x;
  }
};
