#include "core/common/Bindless.hlsl"
BINDLESS_BINDINGS(1, 2, 3, 4)
#ifdef TEST_LARGE_CONSTANTS
struct DescriptorPadding { uint4 values[14]; };
#define TEST_CONSTANTS(X) X(uint, index) X(uint, iteration) X(DescriptorPadding, padding)
#else
#define TEST_CONSTANTS(X) X(uint, index) X(uint, iteration)
#endif
DEFINE_SHADER_CONSTANTS(TEST_CONSTANTS)

struct TestParameters { uint4 value; };
#ifdef TEST_SEPARATE_UNIFORM_SET
// Only the UB is live in set 5 for BindlessMain; gaps must not disqualify it.
[[vk::binding(2, 5)]] ConstantBuffer<TestParameters> parameters;
#else
[[vk::binding(0, 0)]] ConstantBuffer<TestParameters> parameters;
#endif
// A gap in binding numbers exercises empty layout entries.
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> result;
[[vk::binding(0, 5)]] Buffer<uint> sources[40];
[[vk::binding(0, 6)]] Buffer<uint> typed_source;
[[vk::binding(0, 7)]] Texture2D<float> depth_source;

[numthreads(1, 1, 1)]
void BoundMain() {
    result[moer_constants.iteration % 192] = parameters.value.x + moer_constants.iteration + uint(depth_source.Load(int3(0, 0, 0)) * 4);
}

[numthreads(1, 1, 1)]
void BindlessMain() {
    result[moer_constants.iteration % 192] = parameters.value.x + moer_constants.iteration + typed_source[0] + uint(depth_source.Load(int3(0, 0, 0)) * 4) +
        ArrayBuffer(moer_constants.index).Load<uint>(0) +
        uint(TextureHandle(moer_constants.index + 1).SampleLevel<float4>(float2(0.5, 0.5), 0).x * 255.0 + 0.5);
#ifdef TEST_LARGE_CONSTANTS
    result[moer_constants.iteration % 192] += moer_constants.padding.values[13].x;
#endif
}

[numthreads(1, 1, 1)]
void ArrayMain() {
    result[moer_constants.iteration % 192] = sources[moer_constants.iteration % 40][0] + moer_constants.iteration;
}
