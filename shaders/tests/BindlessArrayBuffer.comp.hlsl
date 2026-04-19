#include <core/common/Bindless.hlsl>

BINDLESS_BINDINGS(0)

[numthreads(1, 1, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
  ArrayBuffer array_buffer = ArrayBuffer(0);
  uint raw = array_buffer.Load<uint>(dispatch_thread_id.x);
  uint sink = raw;
  sink += 1u;
}