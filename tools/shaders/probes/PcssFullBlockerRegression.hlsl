#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/raster/deferred/lighting/shadows/PCSS.hlsli"

// Isolate the search/filter contract from quad sharing. Unroll only this
// constant probe so DXC evaluates the production PCSS macros without a GPU.
#undef PCSS_SHARE_PER_PIXEL_QUAD
#define PCSS_SHARE_PER_PIXEL_QUAD 0
#define loop unroll

float GetSearchRadiusUVRegression(DirectionalShadowContext ctx) {
    return GetSearchRadiusUVDir(ctx);
}

float3 GetShadowSamplingPosRegression(DirectionalShadowContext ctx, float2 offset_uv) {
    return float3(offset_uv * ctx.shadowMapSize, 0.0);
}

float SampleShadowDepthRegression(DirectionalShadowContext ctx, float3 position) {
    // An 11-texel disk covers all 8-texel search samples, but not the
    // 32-texel PCF footprint. Uncovered shadow texels contain reverse-Z zero.
    return dot(position.xy, position.xy) < 11.0 * 11.0 ? 0.6 : 0.0;
}

float LinearizeShadowDepthRegression(DirectionalShadowContext ctx, float raw_depth) {
    return LinearizeShadowDepthDir(ctx, raw_depth);
}

float GetFragmentDepthRegression(DirectionalShadowContext ctx) {
    return GetFragmentDepthDir(ctx);
}

bool IsShadowedRegression(float occluder_depth, float fragment_depth, float bias) {
    return IsShadowedDir(occluder_depth, fragment_depth, bias);
}

float CalculatePenumbraRegression(DirectionalShadowContext ctx, float blocker_depth) {
    return CalculatePenumbraDir(ctx, blocker_depth);
}

float ClampPenumbraRegression(DirectionalShadowContext ctx, float penumbra_uv) {
    return ClampPenumbraDir(ctx, penumbra_uv);
}

DEFINE_FIND_BLOCKER(Regression, DirectionalShadowContext)
DEFINE_CALCULATE_PCF(Regression, DirectionalShadowContext)
DEFINE_CALCULATE_PCSS(Regression, DirectionalShadowContext)

#undef loop

float main() : SV_Target {
    DirectionalShadowContext ctx = (DirectionalShadowContext)0;
    ctx.receiverDepthNdc = 0.2;
    ctx.shadowMapSize = 4096.0;
    ctx.orthographicWidth = 100.0;
    ctx.depthRangeWorld = 200.0;
    ctx.lightAngleTangent = 0.01;
    ctx.normal = float3(0.0, 0.0, 1.0);
    ctx.lightDir = float3(0.0, 0.0, -1.0);
    return CalculatePcssRegression(ctx);
}
