#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/RasterCommon.hlsli"

#include "materials/Brdf.hlsli"
#include "pipelines/raster/deferred/lighting/Lighting.hlsli"
#include "pipelines/raster/deferred/lighting/shadows/CSM.hlsli"

#include "shared/raster/ShaderParameters.h"

[[vk::binding(0, 0)]] ConstantBuffer<Moer::LightingData> lighting_data;
[[vk::push_constant]] ConstantBuffer<Moer::MaterialPassBindlessParam> param;

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: GBuffer
    float3 albedo = TextureHandle(param.gbuffer_base_color).Sample2D<float3>(in_uv);
    float3 metal_rough_ao = TextureHandle(param.gbuffer_metal_rough_ao).Sample2D<float3>(in_uv);
    float  depth = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);
    float3 N = normalize(
        Raster::UnpackNormal(TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv))
    ); // 因为法线mipmap不满足线性关系，所以这里需要normalize
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.clip2world);
    float metallic = metal_rough_ao.x;
    float roughness = metal_rough_ao.y;

    // - Lights
    ArrayBuffer light_buf = ArrayBuffer(param.light_buf_hdl);

    // MARK: Skybox(Deprecated)
    if (depth == 0.0) {
        return float4(1.0, 0.0, 0.0, 1.0);
    }
    float3 V   = normalize(lighting_data.camera_position - position.xyz);
    float  NoV = saturate(dot(N, V));

    BRDFContext brdf_ctx;
    brdf_ctx.Init(
        roughness,
        albedo,
        metallic,
        N,
        V,
        TextureHandle(lighting_data.lut_ggx_emu_handle).Sample2D<float3>(float2(NoV, roughness)),
        TextureHandle(lighting_data.lut_ggx_eavg_handle).Sample2D<float3>(float2(0.0, roughness))
    );

    brdf_ctx.SetConfig(
        lighting_data.brdf_enable_multi_scatter,
        lighting_data.brdf_NDF_mode,
        lighting_data.brdf_G_mode,
        lighting_data.brdf_G_is_ibl
    );

    // - Shadow
    float shadow = TextureHandle(param.shadow_mask_handle).Sample2D<float>(in_uv);

    // MARK: Shading
    LightContext light_ctx;
    light_ctx.Init(brdf_ctx, position, lighting_data.lut_ggx_emu_handle);

    for (uint i = 0; i < lighting_data.light_count; i++) {
        Moer::GLight light = light_buf.Load<Moer::GLight>(i);

        light_ctx.AccumulateLight(light, shadow);
    }

    float3 color = light_ctx.GetResult();

    if (param.enable_extra_ambient) {
        color += param.extra_ambient_intensity * param.extra_ambient_color * brdf_ctx.albedo;
    }

    color = max(color, 0.0);

    if (
        lighting_data.shadow_csm_visualize_cascade != 0
        && (lighting_data.shadow_map_mode == Moer::EShadowMapMode::CSM
            || lighting_data.shadow_map_mode == Moer::EShadowMapMode::CSM_AUTO)
    ) {
        color = get_cascade_visualize_color(lighting_data, position);
    }

    return float4(color, 1.0);
}