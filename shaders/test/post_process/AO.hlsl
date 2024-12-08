/**
  * SSAO implementation
  * Reference: GAMES202
  */

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

struct Constant {
    uint   ao_mode;
    uint   input_image;
    uint   normal_tex;
    uint   depth_tex;
    uint   position_tex;
    uint   noise_tex;// linear & repeat sampler
    float2 inv_resolution;
    uint   ssao_sample_count;
    uint   ssao_radius;
};
[[vk::push_constant]] ConstantBuffer<Constant> param;

#define AO_MODE_NONE 0
#define AO_MODE_SSAO_IQ 1
#define AO_MODE_SSAO_IQ_AO_ONLY 2
#define AO_MODE_SSAO_GAMES202 3
#define AO_MODE_SSAO_GAMES202_AO_ONLY 4
#define AO_MODE_SSDO_GAMES202 5
#define AO_MODE_SSDO_GAMES202_AO_ONLY 6

#define SSAO_IQ_DEPTH_THRESHOLD 0.1

static const float Epsilon = 0.0001; // same with PBRMaterialFrag.hlsl
static const float3 ABNORMAL_COLOR = float3(0.0, 0.0, 1.0);

// uv in [0, 1]; output in [0, 1]
float2 random_2to2(float2 uv) {
    return TextureHandle(param.noise_tex).Sample2D<float4>(uv).rg;
}
float3 random_2to3(float2 uv) {
    return TextureHandle(param.noise_tex).Sample2D<float4>(uv).rga;
}

float get_depth(float2 uv) {
    float depth = TextureHandle(param.depth_tex).Sample2D<float>(uv).x;

    // TODO: convert to linear depth

    return depth;
}

// The MIT License
// Copyright © 2014 Inigo Quilez
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// Reference: https://www.shadertoy.com/view/Ms23Wm
float ssao_iq(float2 uv) {
    // sample zbuffer (in linear eye space) at the current shading point	
	float zr = 1.0 - get_depth(uv);

    // sample neighbor pixels
	float ao = 0.0;
	for (int i = 0; i < param.ssao_sample_count; i++) {
        // get a random 2D offset vector
        float2 offset = random_2to2(uv + float2(0.02371*i, 0.01337*i)) * 2.0 - 1.0;
        // sample the zbuffer at a neightbor pixel (in a ssao_radius pixel radious)        		
        float z = 1.0 - get_depth(uv + floor(offset * param.ssao_radius) * param.inv_resolution);
        // accumulate occlusion if difference is less than SSAO_IQ_DEPTH_THRESHOLD units		
		ao += clamp((zr - z) / SSAO_IQ_DEPTH_THRESHOLD, 0.0, 1.0);
	}
    // average down the occlusion	
    ao = clamp(1.0 - ao / param.ssao_sample_count, 0.0, 1.0);
	
	return ao;
}

float ssao_games202(float2 uv) {
    float sum = 0.0;
    float3 normal = TextureHandle(param.normal_tex).Sample2D<float4>(uv).rgb * 2.0 - 1.0;

    for (uint i = 0; i < param.ssao_sample_count; i++) {
        float3 offset = random_2to3(uv) * 2.0 - 1.0;
        if (dot(offset, normal) < 0.0) {
            offset = -offset;
        }
    }

    return 1.0;
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {

    float3 color = TextureHandle(param.input_image).Sample2D<float4>(uv).rgb;

    if (param.ao_mode == AO_MODE_NONE) {
        return float4(color, 1.0);

    } else if (param.ao_mode == AO_MODE_SSAO_IQ) {
        float3 ssao_result = ssao_iq(uv);
        return float4(ssao_result * color, 1.0);

    } else if (param.ao_mode == AO_MODE_SSAO_IQ_AO_ONLY) {
        float3 ssao_result = ssao_iq(uv);
        return float4(ssao_result, 1.0);

    } else {
        return float4(ABNORMAL_COLOR, 1.0);
    }
}