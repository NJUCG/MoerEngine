// #include "features/3dgs/3dgs.hlsl"

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> src : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> dst : register(u1, space0);

struct Params {
    uint timestep;
};

[[vk::push_constant]] ConstantBuffer<Params> params : register(b0);

uint power(uint base, uint exponent) {
    uint result = 1;
    for (uint i = 0; i < exponent; i++) {
        result *= base;
    }
    return result;
}

[numthreads(256, 1, 1)] void main(uint3 dispatchThreadId
                                  : SV_DispatchThreadID) {
    uint index = dispatchThreadId.x;
    uint timestep = params.timestep;
    uint length, stride;
    src.GetDimensions(length, stride);
    if (index >= length) {
        return;
    }

    if (timestep % 2 == 0) {
        if (index < pow(2, timestep)) {
            dst[index] = src[index];
        } else {
            uint index2 = index - power(2, timestep);
            dst[index]  = src[index] + src[index2];
        }
    } else {
        if (index < pow(2, timestep)) {
            src[index] = dst[index];
        } else {
            uint index2 = index - power(2, timestep);
            src[index]  = dst[index] + dst[index2];
        }
    }
}
