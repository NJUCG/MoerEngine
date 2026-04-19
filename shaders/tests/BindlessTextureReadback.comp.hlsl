#include <core/common/Bindless.hlsl>

struct Args {
  uint handle0;
  uint handle1;
  uint output_offset;
  uint sample_count;
  float uv0_x;
  float uv0_y;
  float uv1_x;
  float uv1_y;
  float mip0;
  float mip1;
};

[[vk::push_constant]] ConstantBuffer<Args> args;
RWStructuredBuffer<uint> output_buffer;

BINDLESS_BINDINGS(1)

[numthreads(2, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint sample_idx = dispatch_thread_id.x;
  if (sample_idx >= args.sample_count) {
    return;
  }

  uint handle = sample_idx == 0 ? args.handle0 : args.handle1;
  float2 uv = sample_idx == 0 ? float2(args.uv0_x, args.uv0_y) : float2(args.uv1_x, args.uv1_y);
  float mip = sample_idx == 0 ? args.mip0 : args.mip1;

  float4 sampled = TextureHandle(handle).SampleLevel(uv, mip);
  uint red_unorm8 = (uint)round(saturate(sampled.r) * 255.0f);
  output_buffer[args.output_offset + sample_idx] = red_unorm8;
}
