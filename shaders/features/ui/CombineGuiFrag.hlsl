struct PSInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> scene_color : register(t0);
[[vk::binding(0, 1)]] SamplerState linear_sampler : register(s0);

struct SceneRect {
  float2 min_xy;
  float2 max_xy;
};

[[vk::push_constant]] ConstantBuffer<SceneRect> scene_rect;

float4 main(PSInput input) : SV_TARGET {
  if (any(input.uv < scene_rect.min_xy) || any(input.uv > scene_rect.max_xy)) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  float2 scene_uv =
      (input.uv - scene_rect.min_xy) / (scene_rect.max_xy - scene_rect.min_xy);
  // #if VULKAN
  //   scene_uv.y = 1.0 - scene_uv.y;
  // #endif
  return scene_color.Sample(linear_sampler, scene_uv);
}
