
#include "features/ui/Gui.hlsli"
struct VS_INPUT {
  float2 pos : POSITION;
  float2 uv : TEXCOORD0;
  float4 col : COLOR0;
};

GuiVertexOutput main(VS_INPUT input, uint id : SV_INSTANCEID) {
  GuiVertexOutput output;
  output.pos = mul(moer_constants.param.mvp, float4(input.pos.xy, 0.f, 1.f));
  output.col = input.col;
  output.uv = input.uv;
  output.id = id;
  // printf("id %d\n", id);
  return output;
}
