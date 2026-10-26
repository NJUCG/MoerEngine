#include "shared/gaussian/GaussianShadowParameters.h"
[[vk::push_constant]] ConstantBuffer<Moer::GaussianShadowDraw> param;
void main(float4 position:SV_Position, float2 delta:TEXCOORD0, nointerpolation float4 conic:TEXCOORD1) {
    float power = -.5*(conic.x*delta.x*delta.x+2*conic.y*delta.x*delta.y+conic.z*delta.y*delta.y);
    clip(min(.99,conic.w*exp(power))-param.options.x);
}
