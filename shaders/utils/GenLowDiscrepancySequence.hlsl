#include <MathLib/STL.hlsli>
#include <shared/utils/ShaderParameters.h>

[[vk::push_constant]] ConstantBuffer<Moer::GenLowDiscrepancySequenceParam> param : register(b0);

RWBuffer<float2> output : register(u0);

[numthreads(256, 1, 1)]

void main(uint gtid: SV_DISPATCHTHREADID){

    if(gtid >= param.num_samples){
        return;
    }
    [branch] if(param.num_dimensions != 2){
        return;
    }
    
    float2 result = STL::Sequence::Hammersley2D(gtid, param.num_samples) * 2.0f - 1.0f;
    output[gtid] = result;
}