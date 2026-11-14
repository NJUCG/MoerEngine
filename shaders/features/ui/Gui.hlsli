#include "core/common/Bindless.hlsl"

struct GuiVertexOutput {
  float4 pos : SV_POSITION;
  float4 col : COLOR0;
  float2 uv : TEXCOORD0;
  uint id : INSTANCEID;
};

struct DrawParam {
  float2 min_xy;
  float2 max_xy;
  uint image_handle;
  uint padding;
  uint padding2;
  uint padding3;
};
struct Constant {
  float4x4 mvp;
};
BINDLESS_BINDINGS(1, 2, 3, 4)

StructuredBuffer<DrawParam> arg_buffer : register(t0, space0);
DEFINE_SHADER_CONSTANT(Constant, param)
