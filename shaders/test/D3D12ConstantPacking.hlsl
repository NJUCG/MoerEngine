#include "shared/ShaderConstants.h"
#include "shared/test/D3D12ConstantPacking.h"
DEFINE_SHADER_CONSTANT(Moer::D3D12PackingData, packed)
ConstantBuffer<Moer::D3D12PackingData> packed_buffer;
RWStructuredBuffer<uint> values;

uint Evaluate(Moer::D3D12PackingData data) {
    return uint(data.index + data.exposure.z + data.camera.y + data.transform[1][0] + data.samples[2]);
}
[numthreads(1, 1, 1)]
void main() {
    values[0] = Evaluate(moer_constants.packed);
    values[1] = Evaluate(packed_buffer);
}
