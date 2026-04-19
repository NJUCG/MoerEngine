struct PSInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> scene_color : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> gui_color : register(t1);
[[vk::binding(0, 1)]] SamplerState linear_sampler : register(s0);

struct SceneRect {
  float2 min_xy;
  float2 max_xy;
};

[[vk::push_constant]] ConstantBuffer<SceneRect> scene_rect;

float4 main(PSInput input) : SV_TARGET {
  if (any(input.uv < scene_rect.min_xy) || any(input.uv > scene_rect.max_xy)) {
    return gui_color.Sample(linear_sampler, input.uv);
  }
  float2 scene_uv =
      (input.uv - scene_rect.min_xy) / (scene_rect.max_xy - scene_rect.min_xy);
  return scene_color.Sample(linear_sampler, scene_uv);
}