#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  ArrayBuffer array_buffer = ArrayBuffer(0);
  TextureHandle texture = TextureHandle(0);
  SamplerHandle sampler = SamplerHandle(0);

  uint raw = array_buffer.Load<uint>(dispatch_thread_id.x);
  SamplerState bindless_sampler = sampler.GetSampler();
  float4 sampled_a = texture.SampleLevel(float2(0.5, 0.5), 0.0f);
  float4 sampled_b = texture.GetTexture2D().SampleLevel(bindless_sampler, float2(0.25, 0.75), 0.0f);
  uint sink = raw + asuint(sampled_a.x) + asuint(sampled_b.x);
  sink += dispatch_thread_id.x;
}