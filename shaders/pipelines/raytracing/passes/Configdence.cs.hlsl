#include <core/math/Math.hlsli>
#include <shared/ShaderParameters.h>
#include <shared/utils/Packing.h>

[[vk::push_constant]] ConstantBuffer<Moer::ConfidenceParams> params;

[[vk::binding(0, 0)]] Texture2DArray<float4> gradients;
[[vk::binding(1, 0)]] Texture2D<float4> motion;
[[vk::binding(2, 0)]] Texture2D<float> prev_diffuse_confidence;
[[vk::binding(3, 0)]] Texture2D<float> prev_specular_confidence;
[[vk::binding(4, 0)]] RWTexture2D<float> diffuse_confidence;
[[vk::binding(5, 0)]] RWTexture2D<float> specular_confidence;
[[vk::binding(6, 0)]] SamplerState spl;
[numthreads(8, 8, 1)]
void main(uint2 dtid: SV_DISPATCHTHREADID){

    if(any(dtid >= params.resolution))
        return;

    float2 grad_pos = (float2(dtid) + 0.5) / DI_GRAD_FACTOR;

    grad_pos *= params.inv_grad_size;

    float4 gradient = gradients.SampleLevel(spl, float3(grad_pos, params.input_buf_idx), 0);
    gradient = max(gradient, 0);

    gradient.zw += DI_GRAD_STORAGE_SCALE * params.darkness_bias;

    float diff_confidence = saturate(1.f - gradient.x / gradient.z);
    float spec_confidence = saturate(1.f - gradient.y / gradient.w);

    diff_confidence = saturate(pow(diff_confidence, params.sensitivity));
    spec_confidence = saturate(pow(spec_confidence, params.sensitivity));

    if(params.blend_factor < 1.f){
        float2 motion = motion[dtid].xy;
        int2 prev_pos = int2(float2(dtid) + .5f + motion);
        if(all(prev_pos >= 0) && all(prev_pos < params.rect_size)){

            const float power = .25f;
            float prev_diff_confidence = prev_diffuse_confidence[prev_pos];
            float prev_spec_confidence = prev_specular_confidence[prev_pos];

            //power all and de-power all
            diff_confidence = pow(diff_confidence, power);
            spec_confidence = pow(spec_confidence, power);

            prev_diff_confidence = pow(prev_diff_confidence, power);
            prev_spec_confidence = pow(prev_spec_confidence, power);

            //blend
            diff_confidence = lerp(diff_confidence, prev_diff_confidence, params.blend_factor);
            spec_confidence = lerp(spec_confidence, prev_spec_confidence, params.blend_factor);

            //de-power all
            diff_confidence = pow(diff_confidence, 1.f / power);
            spec_confidence = pow(spec_confidence, 1.f / power);
        }
    }

    diffuse_confidence[dtid] = diff_confidence;
    specular_confidence[dtid] = spec_confidence;
}