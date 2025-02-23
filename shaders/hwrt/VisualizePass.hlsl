
#include <framework/Bindless.hlsl>
#include <shared/ShaderParameters.h>

#include <framework/DI/GridCommon.hlsli>
#include <framework/Math.hlsli>
#include <shared/utils/Packing.h>

BINDLESS_BINDINGS(1, 2, 3, 4);

[[vk::binding(0, 0)]] ConstantBuffer<Moer::VisualizeParams> param;
[[vk::binding(1, 0)]] Texture2D<float3> direct_lighting : register(t0);
[[vk::binding(3, 0)]] Texture2D<float4> diffuse_lighting : register(t1);
[[vk::binding(4, 0)]] Texture2D<float4> specular_lighting : register(t2);
[[vk::binding(5, 0)]] Texture2D<float> view_depth : register(t3);
[[vk::binding(6, 0)]] Texture2D<float4> emission : register(t4);
[[vk::binding(7, 0)]] RWTexture2D<float4> output : register(u0);

float3 ViewdepthToWorldPos(Moer::ViewParam _view, int2 _pixel_pos,
                           float _view_depth) {
  float2 uv = (float2(_pixel_pos) + 0.5f) * _view.inv_rect;

  float4 clip_pos = float4(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f, 0.5f, 1.f);
  float4 view_pos = mul(_view.clip2view, clip_pos);
  view_pos.xy /= view_pos.z;
  view_pos.zw = float2(1.f, 1.f);
  view_pos.xyz *= _view_depth;
  return mul(_view.view2world, view_pos).xyz;
}

float3 GetNormal(Moer::ViewParam _view, int2 _pixel_pos) {
  float2 v = (float2(_pixel_pos) + 0.5f) * _view.inv_rect;

  TextureHandle tex_handle =
      (TextureHandle)param.bindless_handles.gbuffer_normal;
  uint normal = tex_handle.SampleLevel<uint>(v, 0);
  return Math::OctToNdirUnorm32(normal);
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

  return rf0_roughness_f;
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
    final_color = float4(diffuse_lighting[pixel_pos].xyz +
                             specular_lighting[pixel_pos].xyz,
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
  case Moer::EFC_MATERIAL:
    final_color = GetMaterialColor(param.main_view, pixel_pos);
    break;
  }

  output[pixel_pos] = final_color;
}
