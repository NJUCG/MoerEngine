struct HiZConfig {
  bool b_mip0;
  uint target_level;
  uint2 size;
};
// size should be power of 2

[[vk::push_constant]] ConstantBuffer<HiZConfig> config : register(b0);

[[vk::binding(0, 0)]] RWTexture2D<float> target : register(u0);
[[vk::binding(0, 1)]] SamplerState depth_sampler : register(t0);
[[vk::binding(0, 2)]] Texture2D<float> depth_buffer : register(t0);

void GenerateMip0(uint3 dtid) {
  uint2 size = config.size;
  uint2 gid = dtid.xy;
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  float2 uv = (gid + 0.5f) / float2(size.x, size.y);
  float depth = depth_buffer.SampleLevel(depth_sampler, uv, 0);
  // reverse y coord
  gid.y = size.y - gid.y - 1;
  target[gid] = depth;
}

void GenerateMipN(uint level, uint3 dtid) {
  uint last_level = level - 1;

  uint2 size = config.size; // example: target mip1 use mip0 size dispatch
  uint2 gid = dtid.xy;
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }
  uint2 coord = gid << 1;
  float4 depth =
      float4(depth_buffer.Load(int3(coord, last_level)),
             depth_buffer.Load(int3(coord + int2(1, 0), last_level)),
             depth_buffer.Load(int3(coord + int2(0, 1), last_level)),
             depth_buffer.Load(int3(coord + int2(1, 1), last_level)));

  depth.xy = min(depth.xy, depth.zw);
  depth.x = min(depth.x, depth.y);
  target[gid] = depth.x;
}

[numthreads(8, 8, 1)] void main(uint3 dtid
                                : SV_DispatchThreadID) {
  [branch] if (config.b_mip0) { GenerateMip0(dtid); }
  else {
    GenerateMipN(config.target_level, dtid);
  }
}