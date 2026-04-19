#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  TextureHandle texture = TextureHandle(dispatch_thread_id.x);
  float4 sampled = texture.SampleLevel(float2(0.5, 0.5), 0.0f);
  uint sink = asuint(sampled.x);
  sink += dispatch_thread_id.x;
}