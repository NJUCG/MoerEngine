#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(1, 2, 3, 4)

#if MOER_TEST_ADDRESS_ONLY
DEFINE_SHADER_BINDLESS_CONSTANTS()
#define TEST_INDEX 1
#define TEST_SCALE 1.0
#elif MOER_TEST_SINGLE_CONSTANT
DEFINE_SHADER_CONSTANT(uint, index)
#define TEST_INDEX moer_constants.index
#define TEST_SCALE 1.0
#else
#define TEST_CONSTANTS(X) X(uint, index) X(float, scale)
DEFINE_SHADER_CONSTANTS(TEST_CONSTANTS)
#define TEST_INDEX moer_constants.index
#define TEST_SCALE moer_constants.scale
#endif

[[vk::binding(0, 0)]] RWStructuredBuffer<float> result;

float ReadBindless(uint index) {
    return float(ArrayBuffer(index).Load<uint>(0)) +
        TextureHandle(index + 1).SampleLevel<float4>(float2(0.5, 0.5), 0).x;
}

[numthreads(1, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID) {
    result[id.x] = TEST_SCALE * ReadBindless(TEST_INDEX + id.x);
}

// The selector is itself a GPU buffer. The graph declares all reachable
// candidates; it does not need to know which index the shader will load.
[numthreads(1, 1, 1)]
void IndirectMain() {
    uint selected = ArrayBuffer(TEST_INDEX).Load<uint>(0);
    result[0] = TEST_SCALE * ReadBindless(selected);
}

float4 VertexMain(uint id : SV_VertexID) : SV_Position {
    return float4(TEST_SCALE * ReadBindless(TEST_INDEX + id), 0, 0, 1);
}

float4 PixelMain(float4 position : SV_Position) : SV_Target0 {
    return TEST_SCALE * ReadBindless(TEST_INDEX + uint(position.x));
}

[numthreads(1, 1, 1)]
void NoBindlessMain() {
    result[0] = TEST_SCALE + TEST_INDEX;
}

#if VULKAN
[numthreads(1, 1, 1)]
void IndexOnlyMain() {
    result[0] = LoadBindlessIndex(TEST_INDEX);
}
#endif
