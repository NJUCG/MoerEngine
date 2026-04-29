struct Args {
  uint element_count;
  uint addend;
  uint xor_mask;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<Args> args : register(b0);
[[vk::binding(0, 0)]] StructuredBuffer<uint> src : register(t0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> dst : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint index = dispatch_thread_id.x;
  if (index >= args.element_count) {
    return;
  }

  dst[index] = (src[index] + args.addend) ^ args.xor_mask;
}
