// 1. basic rhi

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

struct Constant {
    uint color_tex; // initial input image
    uint search_tex;
    uint area_tex;
    uint edges_tex;
    uint blend_tex;
    uint point_sampler;
    uint linear_sampler;
    float4 rt_metrics; // float4(inv_resolution.xy, resolution.xy)
};
[[vk::push_constant]] ConstantBuffer<Constant> param;

// 2. extracting bindless sampler & textures

SamplerState SMAAGetSampler(uint sampler_idx) {
    return gsampler__114514_bdls[NonUniformResourceIndex(sampler_idx)];
}

Texture2D SMAAGetTexture2D(uint handle_idx) {
    uint tex_handle = g__array_114514_bdls[NonUniformResourceIndex(handle_idx)];
    uint tex_idx = tex_handle >> 8;
    Texture2D tex = Texture2D(gTexture2Dfloat4__114514_bdls[NonUniformResourceIndex(tex_idx)]);
    return tex;
}

// 3. smaa settings

// #define SMAA_RT_METRICS float4(1.0 / 1280.0, 1.0 / 720.0, 1280.0, 720.0)
#define SMAA_RT_METRICS param.rt_metrics
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL

// 4. smaa porting functions (custom shading language for bindless rhi)

#if defined(SMAA_CUSTOM_SL)
#define PointSampler gsampler__114514_bdls[NonUniformResourceIndex(param.point_sampler)]
#define LinearSampler gsampler__114514_bdls[NonUniformResourceIndex(param.linear_sampler)]
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#endif

// 5. include "SMAA.hlsl"
#include "test/post_process/SMAA.hlsl"

// 6. entry functions

// vertex shader

struct SMAAEdgeDetectionVS_Output {
    float4 Pos : SV_POSITION;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
    [[vk::location(1)]] float4 offset[3] : TEXCOORD1;
};

SMAAEdgeDetectionVS_Output SMAAEdgeDetectionVS_Wrapper(uint VertexIndex : SV_VertexID) {
    SMAAEdgeDetectionVS_Output output = (SMAAEdgeDetectionVS_Output)0;
    output.UV = float2((VertexIndex << 1) & 2, VertexIndex & 2);
    output.Pos = float4(output.UV * 2.0f - 1.0f, 0.0f, 1.0f);
    output.UV.y = 1 - output.UV.y;
    
    SMAAEdgeDetectionVS(output.UV, output.offset);

    return output;
}

struct SMAABlendingWeightCalculationVS_Output {
    float4 Pos : SV_POSITION;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
    [[vk::location(1)]] float2 pixcoord : TEXCOORD1;
    [[vk::location(2)]] float4 offset[3] : TEXCOORD2;
};

SMAABlendingWeightCalculationVS_Output SMAABlendingWeightCalculationVS_Wrapper(uint VertexIndex : SV_VertexID) {
    SMAABlendingWeightCalculationVS_Output output = (SMAABlendingWeightCalculationVS_Output)0;
    output.UV = float2((VertexIndex << 1) & 2, VertexIndex & 2);
    output.Pos = float4(output.UV * 2.0f - 1.0f, 0.0f, 1.0f);
    output.UV.y = 1 - output.UV.y;
    
    SMAABlendingWeightCalculationVS(output.UV, output.pixcoord, output.offset);

    return output;
}

struct SMAANeighborhoodBlendingVS_Output {
    float4 Pos : SV_POSITION;
    [[vk::location(0)]] float2 UV : TEXCOORD0;
    [[vk::location(1)]] float4 offset : TEXCOORD1;
};

SMAANeighborhoodBlendingVS_Output SMAANeighborhoodBlendingVS_Wrapper(uint VertexIndex : SV_VertexID) {
    SMAANeighborhoodBlendingVS_Output output = (SMAANeighborhoodBlendingVS_Output)0;
    output.UV = float2((VertexIndex << 1) & 2, VertexIndex & 2);
    output.Pos = float4(output.UV * 2.0f - 1.0f, 0.0f, 1.0f);
    output.UV.y = 1 - output.UV.y;
    
    SMAANeighborhoodBlendingVS(output.UV, output.offset);

    return output;
}

// pixel shader

float2 SMAALumaEdgeDetectionPS_Wrapper(SMAAEdgeDetectionVS_Output input) : SV_TARGET {
    return SMAALumaEdgeDetectionPS(
        input.UV,
        input.offset,
        SMAAGetTexture2D(param.color_tex)
    );
}

float2 SMAAColorEdgeDetectionPS_Wrapper(SMAAEdgeDetectionVS_Output input) : SV_TARGET {
    return SMAAColorEdgeDetectionPS(
        input.UV,
        input.offset,
        SMAAGetTexture2D(param.color_tex)
    );
}

float2 SMAADepthEdgeDetectionPS_Wrapper(SMAAEdgeDetectionVS_Output input) : SV_TARGET {
    return SMAADepthEdgeDetectionPS(
        input.UV,
        input.offset,
        SMAAGetTexture2D(param.color_tex)
    );
}

float4 SMAABlendingWeightCalculationPS_Wrapper(SMAABlendingWeightCalculationVS_Output input) : SV_TARGET {
    return SMAABlendingWeightCalculationPS(
        input.UV,
        input.pixcoord,
        input.offset,
        SMAAGetTexture2D(param.edges_tex),
        SMAAGetTexture2D(param.area_tex),
        SMAAGetTexture2D(param.search_tex),
        float4(0, 0, 0, 0) // Just pass zero for SMAA 1x, see @SUBSAMPLE_INDICES.
    );
}

float4 SMAANeighborhoodBlendingPS_Wrapper(SMAANeighborhoodBlendingVS_Output input) : SV_TARGET {
    return SMAANeighborhoodBlendingPS(
        input.UV,
        input.offset,
        SMAAGetTexture2D(param.color_tex),
        SMAAGetTexture2D(param.blend_tex)
    );
}