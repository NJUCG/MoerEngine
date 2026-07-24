#include <shared/ShaderParameters.h>

#include <pipelines/raytracing/lighting/lib/restir/GridCommon.hlsli>
#include <core/math/Math.hlsli>
#include <shared/utils/Packing.h>

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
[[vk::binding(11, 0)]] RWTexture2D<float4> output : register(u0);

static const float s_fp16_max = 65504.0f;

float3 SafeNormalize(float3 value) {
  return value * rsqrt(dot(value, value) + 1e-9f);
}

bool IsValidViewDepth(float depth) {
  return depth > 0.0f && depth < s_fp16_max * 0.5f;
}

float3 ClipDepthToWorldPos(Moer::ViewParam view, uint2 pixel_pos,
                           float depth) {
  float2 uv = (float2(pixel_pos) + 0.5f) * view.inv_rect;
  float4 clip_pos =
      float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
  float4 world_pos = mul(view.clip2world, clip_pos);
  return world_pos.xyz / world_pos.w;
}

float4 GetWorldPositionColor(float3 world_pos) {
  return float4(frac(world_pos * 0.025f), 1.0f);
}

float4 GetNormalColor(uint2 pixel_pos) {
  if (!IsValidViewDepth(view_depth.Load(int3(pixel_pos, 0)))) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  float3 decoded =
      Math::OctToNdirUnorm32(normal.Load(int3(pixel_pos, 0)));
  return float4(decoded * 0.5f + 0.5f, 1.0f);
}

float4 GetViewDepthColor(uint2 pixel_pos) {
  float depth = view_depth.Load(int3(pixel_pos, 0));
  if (!IsValidViewDepth(depth)) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  float depth_vis = 1.0f - saturate(log2(depth + 1.0f) / 16.0f);
  return float4(depth_vis.xxx, 1.0f);
}

float4 GetClipDepthColor(uint2 pixel_pos) {
  float depth = clip_depth.Load(int3(pixel_pos, 0));
  return float4(saturate(depth).xxx, 1.0f);
}

float4 GetMotionColor(uint2 pixel_pos) {
  float3 motion_vector = motion.Load(int3(pixel_pos, 0)).xyz;
  float2 encoded_xy = saturate(0.5f + motion_vector.xy / 64.0f);
  float encoded_z = saturate(abs(motion_vector.z) / 64.0f);
  return float4(encoded_xy, encoded_z, 1.0f);
}

float4 GetMaterialColor(uint2 pixel_pos) {
  if (!IsValidViewDepth(view_depth.Load(int3(pixel_pos, 0)))) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  uint packed = specular_roughness.Load(int3(pixel_pos, 0));
  float roughness = Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(packed).a;
  return float4(roughness.xxx, 1.0f);
}

float4 GetNormalRoughnessColor(uint2 pixel_pos) {
  if (!IsValidViewDepth(view_depth.Load(int3(pixel_pos, 0)))) {
    return float4(0.0f, 0.0f, 0.0f, 1.0f);
  }

  float4 packed = normal_roughness.Load(int3(pixel_pos, 0));
  float3 decoded = SafeNormalize(packed.xyz * 2.0f - 1.0f);
  return float4(decoded * 0.5f + 0.5f, 1.0f);
}

float4 VisualizeGrid(uint2 pixel_pos) {
  float depth = view_depth.Load(int3(pixel_pos, 0));
  if (!IsValidViewDepth(depth) ||
      param.grid_params.common_params.cell_size <= 1e-6f) {
    return float4(direct_lighting.Load(int3(pixel_pos, 0)), 1.0f);
  }

  float3 world_pos = ClipDepthToWorldPos(
      param.main_view,
      pixel_pos,
      clip_depth.Load(int3(pixel_pos, 0))
  );
  return float4(
      Moer::Grid::GetVisualizeGridColor(param.grid_params, world_pos),
      1.0f
  );
}

[numthreads(16, 16, 1)]
void main(uint2 pixel_pos : SV_DISPATCHTHREADID) {
  if (any(pixel_pos >= param.output_size)) {
    return;
  }

  float4 final_color;
  switch (param.visualize_mode) {
  case Moer::EFC_SceneColor:
    final_color = float4(direct_lighting[pixel_pos], 1.0f);
    break;
  case Moer::EFC_DI:
    final_color =
        float4(diffuse_lighting[pixel_pos].xyz +
                   specular_lighting[pixel_pos].xyz +
                   emission[pixel_pos].xyz,
               1.0f);
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
    final_color = VisualizeGrid(pixel_pos);
    break;
  case Moer::EFC_POSITION:
    if (!IsValidViewDepth(view_depth[pixel_pos])) {
      final_color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    } else {
      final_color = GetWorldPositionColor(
          ClipDepthToWorldPos(
              param.main_view,
              pixel_pos,
              clip_depth[pixel_pos]
          )
      );
    }
    break;
  case Moer::EFC_NORMAL:
    final_color = GetNormalColor(pixel_pos);
    break;
  case Moer::EFC_VIEW_DEPTH:
    final_color = GetViewDepthColor(pixel_pos);
    break;
  case Moer::EFC_DEPTH:
    final_color = GetClipDepthColor(pixel_pos);
    break;
  case Moer::EFC_MATERIAL:
    final_color = GetMaterialColor(pixel_pos);
    break;
  case Moer::EFC_MOTION:
    final_color = GetMotionColor(pixel_pos);
    break;
  case Moer::EFC_CUSTOM:
    final_color = GetNormalRoughnessColor(pixel_pos);
    break;
  case Moer::EFC_INSTANCE:
  default:
    final_color = float4(direct_lighting[pixel_pos], 1.0f);
    break;
  }

  output[pixel_pos] = final_color;
}
