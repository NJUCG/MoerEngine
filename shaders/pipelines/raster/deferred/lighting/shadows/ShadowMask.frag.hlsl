#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/RasterCommon.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/Shadows.hlsli"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::DirectionalShadowMaskPassBindlessParam> param;

float main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: Lighting Data
    ArrayBuffer        global_params = ArrayBuffer(param.global_param_hdl);
    Moer::LightingData lighting_data = global_params.Load<Moer::LightingData>(0);

    // MARK: GBuffer
    float  depth  = TextureHandle(param.depth_hdl).Sample2D<float>(in_uv);

    // 天空/背景像素在 Reverse-Z 下 depth == 0，无需 PCSS，直接返回全亮
    if (depth < 1e-6)
        return 1.0;

    float3 normal = normalize(Raster::UnpackNormal(TextureHandle(param.normal_hdl).Sample2D<float3>(in_uv)));
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.clip2world);

    float shadow = calculate_shadow(lighting_data, position, in_uv, normal);

    return shadow;
}