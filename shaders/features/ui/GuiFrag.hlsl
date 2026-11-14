#include "features/ui/Gui.hlsli"

float4 main(GuiVertexOutput input) : SV_Target {

  DrawParam draw_param = arg_buffer[input.id];

  if (any(input.pos.xy < float2(draw_param.min_xy.x, draw_param.min_xy.y)) ||
      any(input.pos.xy > float2(draw_param.max_xy.x, draw_param.max_xy.y))) {
    discard;
    return 0.f;
  }
  TextureHandle tex = TextureHandle(draw_param.image_handle);
  float4 out_col = input.col * tex.Sample2D<float4>(input.uv);
  // out_col.a = 0.5f;
  return out_col;
}
