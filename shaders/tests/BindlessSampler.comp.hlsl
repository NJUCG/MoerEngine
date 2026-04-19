#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  SamplerHandle sampler = SamplerHandle(dispatch_thread_id.x);
  SamplerState bindless_sampler = sampler.GetSampler();
  Texture2D<float4> tex = Texture2D<float4>(ResourceDescriptorHeap[0]);
  float4 sample_value = tex.SampleLevel(bindless_sampler, float2(0.5, 0.5), 0.0f);
  uint sink = asuint(sample_value.x);
  sink += dispatch_thread_id.x;
}