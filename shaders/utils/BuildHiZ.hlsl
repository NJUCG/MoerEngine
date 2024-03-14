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
  target[gid] = depth;
}

void GenerateMipN(uint level, uint3 dtid) {
  uint last_level = level - 1;

  uint2 size = config.size; // example: target mip1 use mip0 size dispatch
  uint2 gid = dtid.xy;
  if (gid.x >= size.x || gid.y >= size.y) {
    return;
  }

  float depth = depth_buffer.Load(int3(gid, last_level));
  // float depth_x = QuadReadAcrossX(depth);
  // float depth_y = QuadReadAcrossY(depth);
  // float depth_diag = QuadReadAcrossDiagonal(depth);
  float depth_x = depth_buffer.Load(int3(gid + int2(1, 0), last_level));
  float depth_y = depth_buffer.Load(int3(gid + int2(0, 1), last_level));
  float depth_diag = depth_buffer.Load(int3(gid + int2(1, 1), last_level));
  if (all(gid % 2 == 0)) {
    uint2 target_coord = uint2(gid.x >> 1, gid.y >> 1);
    float min_depth = min(min(depth, depth_x), min(depth_y, depth_diag));
    target[target_coord] = min_depth;
  }
}

[numthreads(8, 8, 1)] void main(uint3 dtid
                                : SV_DispatchThreadID) {
  [branch] if (config.b_mip0) { GenerateMip0(dtid); }
  else {
    GenerateMipN(config.target_level, dtid);
  }
}