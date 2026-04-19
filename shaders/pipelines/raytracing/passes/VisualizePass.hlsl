
#include <core/common/Bindless.hlsl>
#include <shared/ShaderParameters.h>

#include <pipelines/raytracing/lighting/lib/restir/GridCommon.hlsli>
#include <core/math/Math.hlsli>
#include <shared/utils/Packing.h>
#include <shared/nrd/NRDDefinition.h>

BINDLESS_BINDINGS(1);

[[vk::binding(0, 0)]] ConstantBuffer<Moer::VisualizeParams> param;
[[vk::binding(1, 0)]] Texture2D<float3> direct_lighting : register(t0);
[[vk::binding(3, 0)]] Texture2D<float4> diffuse_lighting : register(t1);
[[vk::binding(4, 0)]] Texture2D<float4> specular_lighting : register(t2);
[[vk::binding(5, 0)]] Texture2D<float> view_depth : register(t3);
[[vk::binding(6, 0)]] Texture2D<float4> emission : register(t4);
[[vk::binding(7, 0)]] RWTexture2D<float4> output : register(u0);

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

float3 ViewdepthToWorldPos(Moer::ViewParam _view, int2 _pixel_pos,
                           float _view_depth) {
  float2 uv = (float2(_pixel_pos) + 0.5f) * _view.inv_rect;

  float4 clip_pos = float4(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f, 0.5f, 1.f);
  float4 view_pos = mul(_view.clip2view, clip_pos);
  view_pos.xy /= view_pos.z;
  view_pos.zw = float2(1.f, 1.f);
  view_pos.xyz *= -_view_depth;
  return mul(_view.view2world, view_pos).xyz;
}

float2 GetUV(int2 _pixel_pos, Moer::ViewParam _view) {
  return (float2(_pixel_pos) + 0.5f) * _view.inv_rect;
}

float3 GetNormal(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 v = (float2(_pixel_pos) + 0.5f) * _view.inv_rect;

  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.gbuffer_normal;
  uint normal = tex_handle.SampleLevel<uint>(v, 0);
  return Math::OctToNdirUnorm32(normal);
}

float GetDepth(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 uv = GetUV(_pixel_pos, _view);
  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.gbuffer_depth;
  return tex_handle.SampleLevel<float>(uv, 0);
}

float GetPrevDepth(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 uv = GetUV(_pixel_pos, _view);
  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.gbuffer_prev_depth;
  return tex_handle.SampleLevel<float>(uv, 0);
}

float4 GetViewDepthColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float depth = GetDepth(_view, _pixel_pos);
  if (depth <= 0.0f || depth >= NRD_FP16_MAX * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  float depth_vis = 1.0f - saturate(log2(depth + 1.0f) / 16.0f);
  return float4(depth_vis.xxx, 1.0f);
}

float4 GetMotionColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 uv = GetUV(_pixel_pos, _view);
  TextureHandle tex_handle = (TextureHandle)param.bindless_handles.motion;

  float3 motion = tex_handle.SampleLevel<float3>(uv, 0);
  float2 encoded_xy = saturate(0.5f + motion.xy / 64.0f);
  float encoded_z = saturate(abs(motion.z) / 64.0f);
  return float4(encoded_xy, encoded_z, 1.0f);
}

float4 GetNormalRoughnessColor(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 uv = GetUV(_pixel_pos, _view);
  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.denoiser_normal_roughness;

  float4 packed_normal_roughness = tex_handle.SampleLevel<float4>(uv, 0);
    float4 normal_roughness =
      UnpackDenoiserNormalAndRoughness(packed_normal_roughness);

  return float4(normal_roughness.xyz * 0.5f + 0.5f, 1.0f);
}

void VisualizeGrid(uint2 _pixel_pos, out float4 _final_color) {

  float3 world_pos =
      ViewdepthToWorldPos(param.main_view, _pixel_pos, view_depth[_pixel_pos]);

  _final_color = float4(direct_lighting[_pixel_pos], 1.f);
  float3 dbg_color =
      Moer::Grid::GetVisualizeGridColor(param.grid_params, world_pos);
  _final_color *= float4(dbg_color, 1.f);
  _final_color = float4(dbg_color, 1.f);
}

float4 GetMaterialColor(Moer::ViewParam _view, uint2 _pixel_pos) {
  float2 v = (float2(_pixel_pos) + 0.5f) * _view.inv_rect;

  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.gbuffer_specular_roughness;
  uint rf0_roughness = tex_handle.SampleLevel<uint>(v, 0);
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
    final_color = float4(
        ViewdepthToWorldPos(param.main_view, pixel_pos, view_depth[pixel_pos]),
        1.f);
    break;

  case Moer::EFC_NORMAL:
    final_color = float4(GetNormal(param.main_view, pixel_pos), 1.f);
    break;
  case Moer::EFC_VIEW_DEPTH:
    final_color = GetViewDepthColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_MATERIAL:
    final_color = GetMaterialColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_MOTION:
    final_color = GetMotionColor(param.main_view, pixel_pos);
    break;
  case Moer::EFC_CUSTOM:
    final_color = GetNormalRoughnessColor(param.main_view, pixel_pos);
    break;
  }

  output[pixel_pos] = final_color;
}
