#include "features/gaussian/GaussianMath.hlsli"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianDispatchParams> param;
[[vk::binding(0, 0)]] StructuredBuffer<uint> parent_prefix;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> prefix;
[numthreads(256,1,1)]
void main(uint3 thread_id : SV_DispatchThreadID) {
    uint value_id = thread_id.x, block_id = value_id / 256;
    if (value_id >= param.job.x || block_id == 0) return;
    prefix[value_id] = GaussianSaturatedAdd(prefix[value_id], parent_prefix[block_id - 1], param.info.y);
}
