#include <shared/utils/MoerMath.hlsli>

[[vk::binding(0, 0)]] Texture2D src_color : register(t0);
[[vk::binding(1, 0)]] SamplerState spl : register(s0);
[[vk::binding(2, 0)]] RWTexture2D<float4> dst_color : register(u1);

[numthreads(256, 1, 1)] void main(uint2 gid
                                  : SV_GroupID, uint tid
                                  : SV_GroupThreadID) {
  int2 dst_dim;
  dst_color.GetDimensions(dst_dim.x, dst_dim.y);
  uint2 local_idx = Math::LinearIndexToZCurve(tid);
  uint2 gtid = (gid * 16) + local_idx;
  if (gtid.x >= dst_dim.x || gtid.y >= dst_dim.y) {
    return;
  }
  float2 src_dim;
  src_color.GetDimensions(src_dim.x, src_dim.y);

  float2 uv = (float2(gtid.xy) + 0.5f) / src_dim;
  float2 grad_x = float2(1.f / src_dim.x, 0.f);
  float2 grad_y = float2(0.f, 1.f / src_dim.y);

  dst_color[gtid.xy] = src_color.SampleGrad(spl, uv, grad_x, grad_y);
}