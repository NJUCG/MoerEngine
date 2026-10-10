struct Parameters {
    uint buffer_index;
    uint texture_index;
    uint sampler_index;
};

[[vk::push_constant]] ConstantBuffer<Parameters> params;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> result;
[[vk::binding(0, 1)]] StructuredBuffer<uint> g__array_114514_bdls;
[[vk::binding(1, 1)]] ByteAddressBuffer gbuffer__114514_bdls[];
[[vk::binding(0, 2)]] Texture2D<float4> gtexture__114514_bdls[];
[[vk::binding(0, 3)]] SamplerState gsampler__114514_bdls[];

#if MOER_TEST_REFLECTION_COLLISION
[[vk::binding(1, 0)]] StructuredBuffer<uint> bdls_114514;
#endif

[numthreads(1, 1, 1)]
void ComputeMain() {
    result[0] = g__array_114514_bdls[0] +
        gbuffer__114514_bdls[params.buffer_index].Load<uint>(0) +
        uint(gtexture__114514_bdls[params.texture_index].SampleLevel(
            gsampler__114514_bdls[params.sampler_index], float2(0.5, 0.5), 0).x);
#if MOER_TEST_REFLECTION_COLLISION
    result[0] += bdls_114514[0];
#endif
}
