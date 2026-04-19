#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint packed_handle = g__array_bindless[NonUniformResourceIndex(dispatch_thread_id.x)];
  uint raw = ByteAddressBuffer(ResourceDescriptorHeap[NonUniformResourceIndex(packed_handle)]).Load<uint>(0);
  uint sink = raw;
  sink += dispatch_thread_id.x;
}