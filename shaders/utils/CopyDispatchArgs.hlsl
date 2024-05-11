struct Args {
  uint src_offset;
  uint dst_offset;
  uint group_size;
};
// size should be power of 2

[[vk::push_constant]] ConstantBuffer<Args> args : register(b0);

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> target : register(u0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> src_buffer : register(t0);

[numthreads(1, 1, 1)] void main(uint3 dtid
                                : SV_DispatchThreadID) {
  uint src_offset = args.src_offset >> 2;
  uint dst_offset = args.dst_offset >> 2;
  uint dst_cnt =
      (args.group_size - 1 + src_buffer[src_offset]) / args.group_size;
  target[dst_offset] = dst_cnt;
  uint rest_val = dst_cnt > 0 ? 1 : 0;
  target[dst_offset + 1] = rest_val;
  target[dst_offset + 2] = rest_val;
  // printf("src_offset %d dst_offset %d dst_cnt %d rest_val %d \n", src_offset,
  //        dst_offset, dst_cnt, rest_val);
}