#include "core/common/Bindless.hlsl"
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
[[vk::push_constant]] ConstantBuffer<Constant> param : register(b0);
