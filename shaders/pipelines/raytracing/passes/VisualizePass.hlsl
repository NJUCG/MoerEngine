
#include <shared/ShaderParameters.h>

#include <pipelines/raytracing/lighting/lib/restir/GridCommon.hlsli>
#include <core/math/Math.hlsli>
#include <shared/utils/Packing.h>
#include <shared/nrd/NRDDefinition.h>

[[vk::binding(0, 0)]] ConstantBuffer<Moer::VisualizeParams> param;
[[vk::binding(1, 0)]] Texture2D<float3> direct_lighting : register(t0);
[[vk::binding(2, 0)]] Texture2D<float4> diffuse_lighting : register(t1);
[[vk::binding(3, 0)]] Texture2D<float4> specular_lighting : register(t2);
[[vk::binding(4, 0)]] Texture2D<float> view_depth : register(t3);
[[vk::binding(5, 0)]] Texture2D<float> clip_depth : register(t4);
[[vk::binding(6, 0)]] Texture2D<float4> emission : register(t5);
[[vk::binding(7, 0)]] Texture2D<uint> normal : register(t6);
[[vk::binding(8, 0)]] Texture2D<uint> specular_roughness : register(t7);
[[vk::binding(9, 0)]] Texture2D<float4> motion : register(t8);
[[vk::binding(10, 0)]] Texture2D<float4> normal_roughness : register(t9);
[[vk::binding(11, 0)]] Texture2D<float> prev_view_depth : register(t10);
[[vk::binding(12, 0)]] RWTexture2D<float4> output : register(u0);

float3 SafeNormalize(float3 v) {
  return v * rsqrt(dot(v, v) + 1e-9f);
}

float4 UnpackDenoiserNormalAndRoughness(float4 packed_normal_roughness) {
  float4 decoded = packed_normal_roughness;

  // Current project NRD config uses RGBA8_UNORM normals and linear roughness.
  decoded.xyz = decoded.xyz * 2.0f - 1.0f;
  decoded.xyz = SafeNormalize(decoded.xyz);
  return decoded;
}

float3 GetPrimaryRayDirection(uint2 pixel_pos, Moer::ViewParam view) {
  float2 uv = (float2(pixel_pos) + 0.5f) * view.inv_rect;
  float4 clip_pos =
      float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f / 256.0f, 1.0f);
  float4 world_pos = mul(view.clip2world, clip_pos);
  world_pos.xyz /= world_pos.w;
  return SafeNormalize(world_pos.xyz - view.dir_or_pos.xyz);
}

float4 GetCameraPositionColor(Moer::ViewParam view) {
  return float4(frac(view.dir_or_pos.xyz * 0.01f), saturate(view.dir_or_pos.w));
}

float4 GetPrimaryRayColor(uint2 pixel_pos, Moer::ViewParam view) {
  return float4(GetPrimaryRayDirection(pixel_pos, view) * 0.5f + 0.5f, 1.0f);
}

float4 GetViewParamColor(Moer::ViewParam view) {
  float2 output_size =
      max(float2(param.output_size), float2(1.0f, 1.0f));
  float2 rect_error = abs(view.rect - output_size) / output_size;
  float inv_rect_error =
      length(view.inv_rect * output_size - float2(1.0f, 1.0f));
  float jitter_len = length(view.jitter);
  float error = max(max(rect_error.x, rect_error.y), inv_rect_error);
  return float4(saturate(rect_error.x * 64.0f),
                1.0f - saturate(error * 64.0f),
                saturate(rect_error.y * 64.0f + jitter_len), 1.0f);
}

float4 GetClipDepthColor(uint2 pixel_pos) {
  float depth = clip_depth[pixel_pos];
  return float4(saturate(depth).xxx, 1.0f);
}

bool IsValidViewDepth(float depth) {
  return depth > 0.0f && depth < NRD_FP16_MAX * 0.5f;
}

float4 GetWorldPositionColor(float3 world_pos) {
  return float4(frac(world_pos * 0.025f), 1.0f);
}

float3 ClipDepthToWorldPos(Moer::ViewParam view, uint2 pixel_pos,
                           float depth) {
  float2 uv = (float2(pixel_pos) + 0.5f) * view.inv_rect;
  float4 clip_pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f,
                           depth, 1.0f);
  float4 world_pos = mul(view.clip2world, clip_pos);
  return world_pos.xyz / world_pos.w;
}

float3 ViewdepthToWorldPos(Moer::ViewParam _view, int2 _pixel_pos,
                           float _view_depth) {
  float3 ray_direction = GetPrimaryRayDirection(uint2(_pixel_pos), _view);
  float3 view_direction = mul(_view.world2view, float4(ray_direction, 0.0f)).xyz;
  float ray_t = -_view_depth / view_direction.z;
  return _view.dir_or_pos.xyz + ray_direction * ray_t;
}

float2 GetUV(int2 _pixel_pos, Moer::ViewParam _view) {
  return (float2(_pixel_pos) + 0.5f) * _view.inv_rect;
}

float3 GetNormal(Moer::ViewParam _view, int2 _pixel_pos) {
  uint packed_normal = normal.Load(int3(_pixel_pos, 0));
  return Math::OctToNdirUnorm32(packed_normal);
}

float GetDepth(Moer::ViewParam _view, int2 _pixel_pos) {
  return view_depth.Load(int3(_pixel_pos, 0));
}

float4 GetNormalColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float depth = GetDepth(_view, _pixel_pos);
  if (!IsValidViewDepth(depth)) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  return float4(GetNormal(_view, _pixel_pos) * 0.5f + 0.5f, 1.0f);
}

float GetPrevDepth(Moer::ViewParam _view, int2 _pixel_pos) {
  return prev_view_depth.Load(int3(_pixel_pos, 0));
}

float4 GetViewDepthColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float depth = GetDepth(_view, _pixel_pos);
  if (!IsValidViewDepth(depth)) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  float depth_vis = 1.0f - saturate(log2(depth + 1.0f) / 16.0f);
  return float4(depth_vis.xxx, 1.0f);
}

float4 GetMotionColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float3 motion_vector = motion.Load(int3(_pixel_pos, 0)).xyz;
  float2 encoded_xy = saturate(0.5f + motion_vector.xy / 64.0f);
  float encoded_z = saturate(abs(motion_vector.z) / 64.0f);
  return float4(encoded_xy, encoded_z, 1.0f);
}

float4 GetNormalRoughnessColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float4 packed_normal_roughness = normal_roughness.Load(int3(_pixel_pos, 0));
    float4 normal_roughness =
      UnpackDenoiserNormalAndRoughness(packed_normal_roughness);

  return float4(normal_roughness.xyz * 0.5f + 0.5f, 1.0f);
}

void VisualizeGrid(uint2 _pixel_pos, out float4 _final_color) {
  float depth = view_depth[_pixel_pos];
  if (!IsValidViewDepth(depth)) {
    _final_color = float4(direct_lighting[_pixel_pos], 1.0f);
    return;
  }

  float3 world_pos = ClipDepthToWorldPos(param.main_view, _pixel_pos,
                                         clip_depth[_pixel_pos]);

  _final_color = float4(direct_lighting[_pixel_pos], 1.f);
  float3 dbg_color =
      Moer::Grid::GetVisualizeGridColor(param.grid_params, world_pos);
  _final_color *= float4(dbg_color, 1.f);
  _final_color = float4(dbg_color, 1.f);
}

float4 GetMaterialColor(Moer::ViewParam _view, uint2 _pixel_pos) {
  uint rf0_roughness = specular_roughness.Load(int3(_pixel_pos, 0));
  float4 rf0_roughness_f = Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(rf0_roughness);

  return rf0_roughness_f.a;
}

[numthreads(16, 16, 1)] void main(uint2 dtid
                                  : SV_DISPATCHTHREADID) {
  uint2 pixel_pos = int2(dtid);
  uint2 viewport_size = param.output_size;

  if (any(pixel_pos >= viewport_size)) {
    return;
  }

  float4 final_color = 0.0f;
  switch (param.visualize_mode) {
  case Moer::EFC_SceneColor:
    final_color = float4(direct_lighting[pixel_pos], 1.f);
    break;
  case Moer::EFC_DI:
    final_color =
        float4(diffuse_lighting[pixel_pos].xyz +
                   specular_lighting[pixel_pos].xyz + emission[pixel_pos].xyz,
               1.f);
    break;
  case Moer::EFC_DIFFUSE:
    final_color = diffuse_lighting[pixel_pos];
    break;
  case Moer::EFC_SPECULAR:
    final_color = specular_lighting[pixel_pos];
    break;
  case Moer::EFC_EMISSIVE:
    final_color = emission[pixel_pos];
    break;
  case Moer::EFC_GRID:
    VisualizeGrid(pixel_pos, final_color);
    break;
  case Moer::EFC_POSITION:
    if (!IsValidViewDepth(view_depth[pixel_pos])) {
      final_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
      final_color = GetWorldPositionColor(ClipDepthToWorldPos(
          param.main_view, pixel_pos, clip_depth[pixel_pos]));
    }
    break;

  case Moer::EFC_NORMAL:
    final_color = GetNormalColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_VIEW_DEPTH:
    final_color = GetViewDepthColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_DEPTH:
    final_color = GetClipDepthColor(pixel_pos);
    break;
  case Moer::EFC_MATERIAL:
    final_color = GetMaterialColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_MOTION:
    final_color = GetMotionColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_CAMERA_POSITION:
    final_color = GetCameraPositionColor(param.main_view);
    break;
  case Moer::EFC_PRIMARY_RAY:
    final_color = GetPrimaryRayColor(pixel_pos, param.main_view);
    break;
  case Moer::EFC_VIEW_PARAM:
    final_color = GetViewParamColor(param.main_view);
    break;
  case Moer::EFC_CUSTOM:
    final_color = GetNormalRoughnessColor(param.main_view, pixel_pos);
    break;
  }

  output[pixel_pos] = final_color;
}
