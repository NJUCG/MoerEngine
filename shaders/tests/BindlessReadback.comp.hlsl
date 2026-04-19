#include <core/common/Bindless.hlsl>

struct Args {
  uint src_handle;
  uint xor_mask;
  uint element_count;
};

[[vk::push_constant]] ConstantBuffer<Args> args : register(b0);
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> output_buffer : register(u0);

BINDLESS_BINDINGS(1)

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint index = dispatch_thread_id.x;
  if (index >= args.element_count) {
    return;
  }

  ArrayBuffer src_buffer = ArrayBuffer(args.src_handle);
  uint value = src_buffer.Load<uint>(index);
  output_buffer[index] = value ^ args.xor_mask;
}