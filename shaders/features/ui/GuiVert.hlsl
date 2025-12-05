
#include "features/ui/Gui.hlsli"
struct VS_INPUT {
  float2 pos : POSITION;
  float2 uv : TEXCOORD0;
  float4 col : COLOR0;
};

struct PS_INPUT {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  uint id : INSTANCEID;
};

PS_INPUT main(VS_INPUT input, uint id : SV_INSTANCEID) {
  PS_INPUT output;
  output.pos = mul(param.mvp, float4(input.pos.xy, 0.f, 1.f));
  output.col = input.col;
  output.uv = input.uv;
  output.id = id;
  // printf("id %d\n", id);
  return output;
}