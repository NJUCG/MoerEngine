#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint packed_handle = g__array_bindless[NonUniformResourceIndex(dispatch_thread_id.x)];
  uint texture_index = packed_handle >> 8u;
  uint sampler_index = packed_handle & ((1u << 8u) - 1u);

  Texture2D<float4> texture_value = Texture2D<float4>(ResourceDescriptorHeap[NonUniformResourceIndex(texture_index)]);
  SamplerState sampler_value = (SamplerState)SamplerDescriptorHeap[NonUniformResourceIndex(sampler_index)];
  float4 sampled = texture_value.SampleLevel(sampler_value, float2(0.5, 0.5), 0.0f);
  uint sink = asuint(sampled.x);
  sink += dispatch_thread_id.x;
}