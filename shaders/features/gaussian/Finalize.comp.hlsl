#include "shared/gaussian/GaussianRasterParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint> prefix;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> status;
[numthreads(1,1,1)]
void main() {
    uint pair_count = param.info.x == 0 ? 0 : prefix[param.info.x - 1];
    status[0] = uint4(pair_count, pair_count > param.info.y ? 1 : 0, param.info.x, 0);
}
