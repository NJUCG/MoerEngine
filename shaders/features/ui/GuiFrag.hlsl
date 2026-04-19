#include "features/ui/Gui.hlsli"
struct PS_INPUT {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  uint id : INSTANCEID;
};

float4 main(PS_INPUT input) : SV_Target {

  DrawParam draw_param = arg_buffer[input.id];

  if (any(input.pos.xy < float2(draw_param.min_xy.x, draw_param.min_xy.y)) ||
      any(input.pos.xy > float2(draw_param.max_xy.x, draw_param.max_xy.y))) {
    discard;
    return 0.f;
  }
  TextureHandle tex = TextureHandle(draw_param.image_handle);
  float4 sampled_color = tex.Sample2D<float4>(input.uv);
  float4 out_col = input.col * sampled_color;
  // out_col.a = 0.5f;
  return out_col;
}