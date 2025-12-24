#include "core/math/STL.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::TonemappingPipelineBindlessParam> param;

[[vk::binding(0, 0)]] Buffer<uint> histogram;
[[vk::binding(1, 0)]] RWBuffer<uint> exposure;

[numthreads(1, 1, 1)]
void main() {

    exposure[0] = asuint(param.debug_param);
}