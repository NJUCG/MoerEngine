struct Args {
  uint stride;
  uint component_cnt;
};

struct IndicePair {
  uint src;
  uint dst;
};

[[vk::push_constant]] ConstantBuffer<Args> args : register(b0);

[[vk::binding(0, 0)]] StructuredBuffer<IndicePair> indices : register(t0);
[[vk::binding(0, 1)]] ByteAddressBuffer src : register(t0, space1);
[[vk::binding(1, 1)]] RWByteAddressBuffer dst : register(u0);

[numthreads(64, 1, 1)] void main(uint gid
                                 : SV_DispatchThreadID) {
  if (gid >= args.component_cnt) {
    return;
  }
  IndicePair pair = indices[gid];
  // printf("src %d dst %d stride %d component_cnt %d\n", pair.src, pair.dst,
  //        args.stride, args.component_cnt);

  [branch] if (args.stride == 1) {
    uint src_offset = pair.src * 4;
    uint dst_offset = pair.dst * 4;
    dst.Store(dst_offset, src.Load(src_offset));
  }
  [branch] if (args.stride == 2) {
    uint src_offset = pair.src * args.stride * 4;
    uint dst_offset = pair.dst * args.stride * 4;
    uint2 val = src.Load2(src_offset);
    dst.Store2(dst_offset, val);
  }
  [branch] if (args.stride == 3) {
    uint src_offset = pair.src * args.stride * 4;
    uint dst_offset = pair.dst * args.stride * 4;
    uint3 val = src.Load3(src_offset);
    dst.Store3(dst_offset, val);
  }
  [branch] if (args.stride == 4) {
    uint src_offset = pair.src * args.stride * 4;
    uint dst_offset = pair.dst * args.stride * 4;
    uint4 val = src.Load4(src_offset);
    dst.Store4(dst_offset, val);
  }
  [branch] if (args.stride > 4) {
    uint src_offset = pair.src * args.stride * 4;
    uint dst_offset = pair.dst * args.stride * 4;
    uint step = args.stride >> 2;
    uint rest = args.stride & 3;

    for (uint i = 0; i < step; i++) {
      uint4 val = src.Load4(src_offset + i * 16);
      dst.Store4(dst_offset + i * 16, val);
    }
    [branch] if (rest > 0) {
      [branch] if (rest == 1) {
        uint val = src.Load(src_offset + step * 16);
        dst.Store(dst_offset + step * 16, val);
      }
      [branch] if (rest == 2) {
        uint2 val = src.Load2(src_offset + step * 16);
        dst.Store2(dst_offset + step * 16, val);
      }
      [branch] if (rest == 3) {
        uint3 val = src.Load3(src_offset + step * 16);
        dst.Store3(dst_offset + step * 16, val);
      }
    }
  }
}
