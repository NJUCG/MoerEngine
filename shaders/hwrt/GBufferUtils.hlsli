#ifndef MOER_GBUFFER_UTILS_HLSLI
#define MOER_GBUFFER_UTILS_HLSLI
#include "shared/Geometry.h"
#include "shared/ShaderParameters.h"

namespace Moer {

RayDesc SetupPrimaryRay(uint2 pixelPosition, Moer::ViewParam _view) {
  float2 uv = (float2(pixelPosition) + 0.5) * _view.inv_rect;
  float4 clip_pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.f, 1.f);
  float4 world_pos = mul(_view.clip2world, clip_pos);
  world_pos.xyz /= world_pos.w;

  RayDesc ray;
  ray.Origin = _view.dir_or_pos.xyz;
  ray.Direction = normalize(world_pos.xyz - ray.Origin);
  ray.TMin = 0;
  ray.TMax = 5000;
  return ray;
}

// Get 2.5D motion for denoising
float3 GetMotion(Moer::ViewParam _view, Moer::ViewParam _prev_view,
                 Moer::InstanceData _instance, float3 _model_pos,
                 float3 _model_pos_prev, out float _clip_depth,
                 out float _view_depth) {
  float3 world_pos = mul(_instance.model2world, float4(_model_pos, 1.0f)).xyz;
  float3 world_pos_prev =
      mul(_instance.prev_model2world, float4(_model_pos_prev, 1.0f)).xyz;

  float4 clip_pos = mul(_view.world2clip, float4(world_pos, 1.0f));

  float4 clip_pos_prev =
      mul(_prev_view.world2clip, float4(world_pos_prev, 1.0f));
  clip_pos_prev.xyz /= clip_pos_prev.w;
  clip_pos.xyz /= clip_pos.w;

  _view_depth = clip_pos.w;
  _clip_depth = clip_pos.z;

  if (clip_pos.w <= 0 || clip_pos_prev.w <= 0) {
    return float3(0, 0, 0);
  }

  float3 motion;
  motion.xy = clip_pos_prev.xy - clip_pos.xy;
  motion.z = clip_pos_prev.w - clip_pos.w;

  return motion; // 2.5D motion
}

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

} // namespace Moer

#endif // MOER_GBUFFER_UTILS_HLSLI